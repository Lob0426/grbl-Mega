/*
  stepper.c - stepper motor interface
  Part of Grbl

  Copyright (c) 2009 Simen Svale Skogsrud

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
*/

/* The timer calculations of this module informed by the 'RepRap cartesian firmware' by Zack Smith
   and Philipp Tiefenbacher. The circle buffer implementation gleaned from the wiring_serial library 
   by David A. Mellis */

#include "stepper.h"
#include "config.h"
#include "nuts_bolts.h"
#include <avr/interrupt.h>

#include "wiring_serial.h"

#define TICKS_PER_MICROSECOND (F_CPU/1000000)
#define STEP_BUFFER_SIZE 100

volatile uint8_t step_buffer[STEP_BUFFER_SIZE]; // A buffer for step instructions
volatile int step_buffer_head = 0;
volatile int step_buffer_tail = 0;
volatile uint32_t current_pace;
volatile uint32_t next_pace = 0;

uint8_t stepper_mode = STEPPER_MODE_STOPPED;
uint8_t echo_steps = true;

void config_pace_timer(uint32_t microseconds);

// This timer interrupt is executed at the pace set with set_pace. It pops one instruction from
// the step_buffer, executes it. Then it starts timer2 in order to reset the motor port after
// five microseconds.
SIGNAL(SIG_OUTPUT_COMPARE1A)
{
  if (step_buffer_head != step_buffer_tail) {
    if(step_buffer[step_buffer_tail] == 0xff) {
      // If this is not a step-instruction, but a pace-marker: change pace
      config_pace_timer(next_pace);
      next_pace = 0;
    } else {
      // Set the direction pins a nanosecond or two before you step the steppers
      STEPPING_PORT = (STEPPING_PORT & ~DIRECTION_MASK) | (step_buffer[step_buffer_tail] & DIRECTION_MASK);
      // Then pulse the stepping pins
      STEPPING_PORT = (STEPPING_PORT & ~STEP_MASK) | step_buffer[step_buffer_tail];
      // Reset and start timer 2 which will reset the motor port after 5 microsecond
      TCNT2 = 0; // reset counter
      OCR2A = 5*TICKS_PER_MICROSECOND; // set the time
      TIMSK2 |= (1<<OCIE2A); // enable interrupt
    }
    // move the step buffer tail to the next instruction
    step_buffer_tail = (step_buffer_tail + 1) % STEP_BUFFER_SIZE;
  }
}

// This interrupt is set up by SIG_OUTPUT_COMPARE1A when it sets the motor port bits. It resets
// the motor port after a short period (5us) completing one step cycle.
SIGNAL(SIG_OUTPUT_COMPARE2A)
{
  STEPPING_PORT = STEPPING_PORT & ~STEP_MASK; // reset stepping pins (leave the direction pins)
  TIMSK2 &= ~(1<<OCIE2A); // disable this timer interrupt until next time
}

// Initialize and start the stepper motor subsystem
void st_init()
{
	// Configure directions of interface pins
  STEPPING_DDR   |= STEPPING_MASK;
  LIMIT_DDR &= ~(LIMIT_MASK);
  STEPPERS_ENABLE_DDR |= 1<<STEPPERS_ENABLE_BIT;

	// waveform generation = 0100 = CTC
	TCCR1B &= ~(1<<WGM13);
	TCCR1B |=  (1<<WGM12);
	TCCR1A &= ~(1<<WGM11); 
	TCCR1A &= ~(1<<WGM10);

	// output mode = 00 (disconnected)
	TCCR1A &= ~(3<<COM1A0); 
	TCCR1A &= ~(3<<COM1B0); 
	
	// Configure Timer 2
  TCCR2A = 0; // Normal operation
  TCCR2B = 1<<CS20; // Full speed, no prescaler
  TIMSK2 = 0; // All interrupts disabled
  
  sei();
  
	// start off with a slow pace
  config_pace_timer(20000);
  st_start();
}

void st_buffer_step(uint8_t motor_port_bits)
{
  if (echo_steps) {
    printByte('!'+motor_port_bits);
  }

	int i = (step_buffer_head + 1) % STEP_BUFFER_SIZE;
	
	// If the buffer is full: good! That means we are well ahead of the robot. 
	// Nap until there is room for more steps.
	while(step_buffer_tail == i) { sleep_mode(); }
	
  step_buffer[step_buffer_head] = motor_port_bits;
  step_buffer_head = i;
}

// Block until all buffered steps are executed
void st_synchronize()
{
  if (stepper_mode == STEPPER_MODE_RUNNING) {
    while(step_buffer_tail != step_buffer_head) { sleep_mode(); }    
  } else {
    st_flush();
  }
}

// Cancel all pending steps
void st_flush()
{
  cli();
  step_buffer_tail = step_buffer_head;
  sei();
}

// Start the stepper subsystem
void st_start()
{
  // Enable timer interrupt
	TIMSK1 |= (1<<OCIE1A);
  STEPPERS_ENABLE_PORT |= 1<<STEPPERS_ENABLE_BIT;
  stepper_mode = STEPPER_MODE_RUNNING;
}

// Execute all buffered steps, then stop the stepper subsystem
inline void st_stop()
{
  st_synchronize();
	TIMSK1 &= ~(1<<OCIE1A);
  STEPPERS_ENABLE_PORT &= ~(1<<STEPPERS_ENABLE_BIT);
  stepper_mode = STEPPER_MODE_STOPPED;
}

void st_buffer_pace(uint32_t microseconds)
{
  // Do nothing if the pace in unchanged
  if (current_pace == microseconds) { return; }
  // If the one-element pace buffer is full, flush step buffer
  if (next_pace != 0) {
    st_synchronize();
  }  
  next_pace = microseconds;
  st_buffer_step(0xff);
}

void config_pace_timer(uint32_t microseconds)
{
  uint32_t ticks = microseconds*TICKS_PER_MICROSECOND;
  uint16_t ceiling;
  uint16_t prescaler;
	if (ticks <= 0xffffL) {
		ceiling = ticks;
    prescaler = 0; // prescaler: 0
	} else if (ticks <= 0x7ffffL) {
    ceiling = ticks >> 3;
    prescaler = 1; // prescaler: 8
	} else if (ticks <= 0x3fffffL) {
		ceiling =  ticks >> 6;
    prescaler = 2; // prescaler: 64
	} else if (ticks <= 0xffffffL) {
		ceiling =  (ticks >> 8);
    prescaler = 3; // prescaler: 256
	} else if (ticks <= 0x3ffffffL) {
		ceiling = (ticks >> 10);
    prescaler = 4; // prescaler: 1024
	} else {
	  // Okay, that was slower than we actually go. Just set the slowest speed
		ceiling = 0xffff;
    prescaler = 4;
	}
	// Set prescaler
  TCCR1B = (TCCR1B & ~(0x07<<CS10)) | ((prescaler+1)<<CS10);
  // Set ceiling
  OCR1A = ceiling;
  current_pace = microseconds;
}

int check_limit_switches()
{
  // Dual read as crude debounce
  return((LIMIT_PORT & LIMIT_MASK) | (LIMIT_PORT & LIMIT_MASK));
}

int check_limit_switch(int axis)
{
  uint8_t mask = 0;
  switch (axis) {
    case X_AXIS: mask = 1<<X_LIMIT_BIT; break;
    case Y_AXIS: mask = 1<<Y_LIMIT_BIT; break;
    case Z_AXIS: mask = 1<<Z_LIMIT_BIT; break;
  }
  return((LIMIT_PORT&mask) || (LIMIT_PORT&mask));    
}

// void perform_go_home() 
// {
//   int axis;
//   if(stepper_mode.home.phase == PHASE_HOME_RETURN) {
//     // We are running all axes in reverse until all limit switches are tripped
//     // Check all limit switches:
//     for(axis=X_AXIS; axis <= Z_AXIS; axis++) {
//       stepper_mode.home.away[axis] |= check_limit_switch(axis);
//     }
//     // Step steppers. First retract along Z-axis. Then X and Y.
//     if(stepper_mode.home.away[Z_AXIS]) {
//       step_axis(Z_AXIS);
//     } else {
//       // Check if all axes are home
//       if(!(stepper_mode.home.away[X_AXIS] || stepper_mode.home.away[Y_AXIS])) {
//         // All axes are home, prepare next phase: to nudge the tool carefully out of the limit switches
//         memset(stepper_mode.home.direction, 1, sizeof(stepper_mode.home.direction)); // direction = [1,1,1]
//         set_direction_bits(stepper_mode.home.direction);
//         stepper_mode.home.phase == PHASE_HOME_NUDGE;
//         return;
//       }
//       step_steppers(stepper_mode.home.away);
//     }
//   } else {    
//     for(axis=X_AXIS; axis <= Z_AXIS; axis++) {
//       if(check_limit_switch(axis)) {
//         step_axis(axis);
//         return;
//       }
//     }
//     // When this code is reached it means all axes are free of their limit-switches. Complete the cycle and rest:
//     clear_vector(stepper_mode.position); // By definition this is location [0, 0, 0]
//     stepper_mode.mode = MODE_AT_REST;
//   }
// }

void st_go_home()
{
  // Todo: Perform the homing cycle
}

void st_set_echo(int value)
{
  echo_steps = value;
}
