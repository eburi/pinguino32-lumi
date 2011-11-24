/*
 * LED-Strip SPI - non-blocking
 * Author: Erich Buri <erich.buri@gmail.com>
 *
 * - Uses different buffers for animator and LED-Strip-Writer
 * - Uses common functions for the timer-stuff
 * - Implements SPI-Emulation on multiple ports:
 *   - soft SPI 1: Pin 3 data, Pin 4 clock
 *   - soft SPI 2: Pin 5 data, Pin 6 clock
 * - Adds cdc for debugging
 */
#define DEBUG_MODE NODEBUG
#include <stdlib.h>
#include <typedef.h>
#include <system.c>
#include <delay.c>
#include <spi.c>
#include <__cdc.c>

//////////////////////////////////////////////////////////////////////////////////
// TYPES
//////////////////////////////////////////////////////////////////////////////////
typedef struct _timerContext {
	u32 timer_start;	// us-counter-start
	u32 timer_stop;		// us-counter-stop
	u32 timer_delay;	// ms-delay
} timerContext;

typedef struct _lwnContext {
	u8 data_pin;
	u8 clk_pin;
	u8 state;
	u8 sub_state;
	u32 pixelIndex;
	u32 zeroCounter;
	u8 bitmask;
} lwnContext;

//////////////////////////////////////////////////////////////////////////////////
// CONSTS & VARIABLES
//////////////////////////////////////////////////////////////////////////////////

// Common
#define LEDS 32 * 10
#define ZEROS_NEEDED (3 * ((LEDS + 63) / 64))

u8 Fcp0;				// number of GetCP0Count()'s for one microsecond
u8 pixel_buff_one[LEDS * 3];    // Two buffer's. One of them is currently drawn,
u8 pixel_buff_two[LEDS * 3];	// the other can be edited

// LED Blinker
#define LB_LED_PIN     13
#define LB_S_WAIT_HIGH 1
#define LB_S_WAIT_LOW  2

u8  lb_state;			// LED_Blinker state - to show non-blocking I/O
timerContext lb_timer;	// Timer variables for the LED_Blinker

// LED-Strip Writer variables
#define LW_S_WAIT_TO_WRITE_PIXEL 1
#define LW_S_WAIT_TO_WRITE_ZEROS  2

u8  lw_state;			// This is the state of the LED_Writer State-machine
u8 *lw_buffer;			// Pointer to the buffer that the LED_Writer should draw
u32 lw_pixelIndex;      // current Pixel
u8  lw_zeroCounter;      // counter for zeros to send at the end

// LED-Strip (no-SPI) data

// states
#define LWN_S_WAIT_FOR_CLOCK_LOW  1
#define LWN_S_WAIT_FOR_DATA_SET   2
#define LWN_S_WAIT_FOR_CLOCK_HIGH 3
// sub-state
#define LWN_SS_WRITE_PIXEL  1
#define LWN_SS_WRITE_ZEROS  2

// instance on Pin 3 (data) and Pin 4 (clock)
lwnContext pin34Context;

// instance on Pin 5 (data) and Pin 6 (clock)
lwnContext pin56Context;

// Animator data
#define ANIM_S_WAIT_EVEN 1
#define ANIM_S_WAIT_ODD  2
#define ON_COLOR_R 0x00
#define ON_COLOR_G 0x00
#define ON_COLOR_B 0xFF

#define OFF_COLOR_R 0x00
#define OFF_COLOR_G 0x00
#define OFF_COLOR_B 0x00

u8 *pixels;				// Pointer to the buffer were the Animator can work
u8  anim_state;			// Animator state - to show non-blocking I/O
timerContext anim_timer;// Timer variables for the Animator

//////////////////////////////////////////////////////////////////////////////////
// Timer and other general functions
//////////////////////////////////////////////////////////////////////////////////
void reset_us_timer(timerContext *timer, u32 usDelay) {
	timer->timer_start = GetCP0Count();
	timer->timer_stop = timer->timer_start + usDelay * Fcp0;
}

void start_ms_timer(timerContext *timer, u32 msDelay) {
	// set ms-delay to <delay>
	timer->timer_delay = msDelay;
	reset_us_timer(timer,1000);
}

// check counter. If us-delay is up, decrement ms-delay
// return 1, if time is up, 0, if timer still running
u8 check_timer(timerContext *timer) {
	if(timer->timer_start <= timer->timer_stop) {
		// "normal" case, just wait until GetCP0Count > lb_timer_stop
		if(GetCP0Count() > timer->timer_stop) {
			// us timer is up, reset
			reset_us_timer(timer,1000);
			if( --(timer->timer_delay) == 0 )
				return 1;
		}
	} else {
		// "overflow" case, wait until count is smaller then start 
		// (=> count did the overflow, too) and then until count > stop		
		if( GetCP0Count() < timer->timer_start && GetCP0Count() > timer->timer_stop ) {
			// us timer is up, reset
			reset_us_timer(timer,1000);
			if( --(timer->timer_delay) == 0 )
				return 1;
		}
	}
	return 0;
}

void switch_buffers() {
	if(lw_buffer == pixel_buff_one) {
		lw_buffer = pixel_buff_two;
		pixels = pixel_buff_one;
	} else {
		lw_buffer = pixel_buff_one;
		pixels = pixel_buff_two;
	}
}

//////////////////////////////////////////////////////////////////////////////////
// LED Blinker
//////////////////////////////////////////////////////////////////////////////////
void lb_setup() {
  // LED_Blinker setup
  pinmode(LB_LED_PIN,OUTPUT);
  lb_state = LB_S_WAIT_HIGH;
  start_ms_timer(&lb_timer, 1000);
}

void lb_process() {
	if(lb_state == LB_S_WAIT_HIGH && check_timer(&lb_timer)) {
		// Waiting to go high, and timer is up
		digitalwrite(LB_LED_PIN, HIGH);
		start_ms_timer(&lb_timer, 1000);
		lb_state = LB_S_WAIT_LOW;
	}
	if(lb_state == LB_S_WAIT_LOW && check_timer(&lb_timer)) {
		// Waiting to go high, and timer is up
		digitalwrite(LB_LED_PIN, LOW);
		start_ms_timer(&lb_timer, 500);
		lb_state = LB_S_WAIT_HIGH;
	}
}

//////////////////////////////////////////////////////////////////////////////////
// LED-Strip Writer
//////////////////////////////////////////////////////////////////////////////////
void lw_setup() {
	// start by writting zeros to wake-up latch(s)
	lw_state = LW_S_WAIT_TO_WRITE_ZEROS;
	lw_pixelIndex = 0;
	lw_zeroCounter = ZEROS_NEEDED;

	SPI_init();
	/* 10mhz - faster is not working */
	SPI_clock(GetSystemClock() / SPI_PBCLOCK_DIV8);
	SPI_mode(SPI_MASTER);
	BUFFER = 0x00; // Trigger STATRX
}

void lw_process() {
	if(lw_state == LW_S_WAIT_TO_WRITE_PIXEL && (STATRX)) {
		// ready to write a pixel
		BUFFER = (0x80 | lw_buffer[lw_pixelIndex++]);
		if(lw_pixelIndex >= (LEDS * 3)) {
			// sent'em all
			lw_state = LW_S_WAIT_TO_WRITE_ZEROS;
			lw_zeroCounter = ZEROS_NEEDED;
		}
	}
	if(lw_state == LW_S_WAIT_TO_WRITE_ZEROS && (STATRX)) {
		// ready to write a zero
		BUFFER = 0x00;
		if(--lw_zeroCounter == 0) {
			// done with zeros, write pixels
			lw_state = LW_S_WAIT_TO_WRITE_PIXEL;
			lw_pixelIndex = 0;
		}
	}
}

//////////////////////////////////////////////////////////////////////////////////
// LED-Strip Writer (no-SPI)
//////////////////////////////////////////////////////////////////////////////////
// Code from Arduino
// this is the faster pin-flexible way! the pins are precomputed once only
// for (uint8_t bit=0x80; bit; bit >>= 1) {
//   *clkportreg &= ~clkpin;				// SET CLK LOW
//   if (d & bit) {
//     *mosiportreg |= mosipin;			// SET DATA HIGH
//   } else {
//     *mosiportreg &= ~mosipin;			// SET DATA LOW
//   }
//   *clkportreg |= clkpin;				// SET CLK HIGH
// }
//
//*clkportreg &= ~clkpin;				// SET CLK LOW

void lwn_setup(lwnContext * context) {
	  ///*DEBUG*/CDCprintf("LED-Strip Writer (no-SPI) Setup..\n");

	  // LED-Strip Writer (no-SPI)
	  pinmode(context->data_pin, OUTPUT);
	  pinmode(context->clk_pin, OUTPUT);
	  digitalwrite(context->clk_pin, LOW);

	  context->state = LWN_S_WAIT_FOR_DATA_SET;
	  context->sub_state = LWN_SS_WRITE_ZEROS;

	  context->bitmask = 0x80;
	  context->zeroCounter = ZEROS_NEEDED;
	  context->pixelIndex = 0;
}

void lwn_process(lwnContext * context) {
	u8 byteToSend;

	if(context->state == LWN_S_WAIT_FOR_DATA_SET) {
		if(context->sub_state == LWN_SS_WRITE_PIXEL) {
			byteToSend = (0x80 | lw_buffer[context->pixelIndex]);
			///*DEBUG*/CDCprintf("WRITE pixel idx: %u byte:%u mask: %u\n", lwn_pixelIndex, byteToSend, lwn_bitmask);
		} else {
			byteToSend = 0x00;
			///*DEBUG*/CDCprintf("WRITE zero count: %u mask: %u\n", lwn_zeroCounter, lwn_bitmask);
		}

		// Set data
		digitalwrite(context->data_pin,  (byteToSend & context->bitmask) ? HIGH : LOW);

		// next state
		context->state = LWN_S_WAIT_FOR_CLOCK_HIGH;

		// data preparation
		context->bitmask >>= 1;
		if(context->bitmask == 0) {
			// reset bitmask
			context->bitmask = 0x80;

			// byte done - what's gone be the next byte?
			if(context->sub_state == LWN_SS_WRITE_PIXEL) {
				// we're writing pixels, next pixel
				context->pixelIndex++;

				if(context->pixelIndex == (LEDS * 3)) {
					// all pixels sent
					context->sub_state = LWN_SS_WRITE_ZEROS;
					context->zeroCounter = ZEROS_NEEDED;
				}
			} else {
				// we're writing zeros
				context->zeroCounter--;
				if(context->zeroCounter == 0) {
					// all zeros sent
					context->sub_state = LWN_SS_WRITE_PIXEL;
					context->pixelIndex = 0;
				}
			}
		}
	}

	if(context->state == LWN_S_WAIT_FOR_CLOCK_HIGH) {
		///*DEBUG*/CDCprintf("SET CLK HIGH\n");
		// Set clock low
		digitalwrite(context->clk_pin, HIGH);

		// next state
		context->state = LWN_S_WAIT_FOR_CLOCK_LOW;
	}

	if(context->state == LWN_S_WAIT_FOR_CLOCK_LOW) {
		///*DEBUG*/CDCprintf("SET CLK LOW\n");
		// Set clock low
		digitalwrite(context->clk_pin, LOW);

		// next state
		context->state = LWN_S_WAIT_FOR_DATA_SET;
	}
}


//////////////////////////////////////////////////////////////////////////////////
// Animator
//////////////////////////////////////////////////////////////////////////////////
void anim_setup() {
	u32 i;
	for(i=0; i < LEDS; i++) {
		u32 lidx = i*3;
		pixel_buff_one[lidx]     = ON_COLOR_G;
		pixel_buff_one[lidx + 1] = ON_COLOR_R;
		pixel_buff_one[lidx + 2] = ON_COLOR_B;
	}

	for(i=0; i < LEDS; i++) {
		u32 lidx = i*3;
		pixel_buff_two[lidx]     = OFF_COLOR_G;
		pixel_buff_two[lidx + 1] = OFF_COLOR_R;
		pixel_buff_two[lidx + 2] = OFF_COLOR_B;
	}

	anim_state = ANIM_S_WAIT_EVEN;
	start_ms_timer(&anim_timer, 50);
}

void anim_process() {
	u32 i;
	if(anim_state == ANIM_S_WAIT_EVEN && check_timer(&anim_timer)) {
		// Waiting to light up EVEN
//		for(i=0; i < LEDS; i++) {
//			u32 lidx = i*3;
////			if( (i % 2) == 0) {
//				pixels[lidx]     = ON_COLOR_G;
//				pixels[lidx + 1] = ON_COLOR_R;
//				pixels[lidx + 2] = ON_COLOR_B;
////			} else {
////				pixels[lidx]     = OFF_COLOR_G;
////				pixels[lidx + 1] = OFF_COLOR_R;
////				pixels[lidx + 2] = OFF_COLOR_B;
////			}
//		}
		switch_buffers();
		start_ms_timer(&anim_timer, 1000);
		anim_state = ANIM_S_WAIT_ODD;
	}
	if(anim_state == ANIM_S_WAIT_ODD && check_timer(&anim_timer)) {
		// Waiting to light up ODD
//		for(i=0; i < LEDS; i++) {
//			u32 lidx = i*3;
////			if( (i % 2) == 0) {
//				pixels[lidx]     = OFF_COLOR_G;
//				pixels[lidx + 1] = OFF_COLOR_R;
//				pixels[lidx + 2] = OFF_COLOR_B;
////			} else {
////				pixels[lidx]     = ON_COLOR_G;
////				pixels[lidx + 1] = ON_COLOR_R;
////				pixels[lidx + 2] = ON_COLOR_B;
////			}
//		}
		switch_buffers();
		start_ms_timer(&anim_timer, 1000);
		anim_state = ANIM_S_WAIT_EVEN;
	}
}

//////////////////////////////////////////////////////////////////////////////////
// MAIN Setup & process
//////////////////////////////////////////////////////////////////////////////////
void setup() {
  ///*DEBUG*/CDCprintf("Setup..\n");

  // for delays - CP0Count counts at half the CPU rate
  Fcp0 = GetSystemClock() / 1000000 / 2;   // max = 40 for 80MHz

  // setup-buffers
  lw_buffer = pixel_buff_one;
  pixels = pixel_buff_two;
  

  //lb_setup();
  lw_setup();

  pin34Context.data_pin = 3;
  pin34Context.clk_pin = 4;
  lwn_setup(&pin34Context);

  pin56Context.data_pin = 5;
  pin56Context.clk_pin = 6;
  lwn_setup(&pin56Context);

  anim_setup();
}

void loop() {
  //lb_process();
  lw_process();

  lwn_process(&pin34Context);
  lwn_process(&pin56Context);

  anim_process();
}
