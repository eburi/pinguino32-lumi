/*
 * LED-Strip SPI - non-blocking
 * Author: Erich Buri <erich.buri@gmail.com>
 *
 * - Uses different buffers for animator and LED-Strip-Writer
 * - Uses common functions for the timer-stuff
 */
#define DEBUG_MODE NODEBUG
#include <stdlib.h>
#include <typedef.h>
#include <system.c>
#include <delay.c>
#include <spi.c>

//////////////////////////////////////////////////////////////////////////////////
// TYPES
//////////////////////////////////////////////////////////////////////////////////
typedef struct _timerContext {
	u32 timer_start;	// us-counter-start
	u32 timer_stop;		// us-counter-stop
	u32 timer_delay;	// ms-delay
} timerContext;

//////////////////////////////////////////////////////////////////////////////////
// CONSTS & VARIABLES
//////////////////////////////////////////////////////////////////////////////////

// Common
#define LEDS 32 * 20

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
#define LW_ZEROS_NEEDED (3 * ((LEDS + 63) / 64))

u8  lw_state;			// This is the state of the LED_Writer State-machine
u8 *lw_buffer;			// Pointer to the buffer that the LED_Writer should draw
u32 lw_pixelIndex;      // current Pixel
u8  lw_zeroCounter;      // counter for zeros to send at the end

// Animator variables
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
void reset_us_timer(timerContext *timer) {
	timer->timer_start = GetCP0Count();
	timer->timer_stop = timer->timer_start + 1000 * Fcp0;
}

void start_ms_timer(timerContext *timer, u32 msDelay) {
	// set ms-delay to <delay>
	timer->timer_delay = msDelay;
	reset_us_timer(timer);
}

// check counter. If us-delay is up, decrement ms-delay
// return 1, if time is up, 0, if timer still running
u8 check_timer(timerContext *timer) {
	if(timer->timer_start < timer->timer_stop) {
		// "normal" case, just wait until GetCP0Count > lb_timer_stop
		if(GetCP0Count() > timer->timer_stop) {
			// us timer is up, reset
			reset_us_timer(timer);
			if( --(timer->timer_delay) <= 0 )
				return 1;
		}
	} else {
		// "overflow" case, wait until count is smaller then start 
		// (=> count did the overflow, too) and then until count > stop		
		if( GetCP0Count() < timer->timer_start && GetCP0Count() > timer->timer_stop ) {
			// us timer is up, reset
			reset_us_timer(timer);
			if( --(timer->timer_delay) <= 0 )
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
	lw_zeroCounter = LW_ZEROS_NEEDED;

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
			lw_zeroCounter = LW_ZEROS_NEEDED;
		}
	}
	if(lw_state == LW_S_WAIT_TO_WRITE_ZEROS && (STATRX)) {
		// ready to write a zero
		BUFFER = 0x00;
		if(--lw_zeroCounter <= 0) {
			// done with zeros, write pixels
			lw_state = LW_S_WAIT_TO_WRITE_PIXEL;
			lw_pixelIndex = 0;
		}
	}
}

//////////////////////////////////////////////////////////////////////////////////
// Animator
//////////////////////////////////////////////////////////////////////////////////
void anim_setup() {
	anim_state = ANIM_S_WAIT_EVEN;
	start_ms_timer(&anim_timer, 50);
}

void anim_process() {
	u32 i;
	if(anim_state == ANIM_S_WAIT_EVEN && check_timer(&anim_timer)) {
		// Waiting to light up EVEN
		for(i=0; i < LEDS; i++) {
			u32 lidx = i*3;
//			if( (i % 2) == 0) {
				pixels[lidx]     = ON_COLOR_G;
				pixels[lidx + 1] = ON_COLOR_R;
				pixels[lidx + 2] = ON_COLOR_B;
//			} else {
//				pixels[lidx]     = OFF_COLOR_G;
//				pixels[lidx + 1] = OFF_COLOR_R;
//				pixels[lidx + 2] = OFF_COLOR_B;
//			}
		}
		switch_buffers();
		start_ms_timer(&anim_timer, 1000);
		anim_state = ANIM_S_WAIT_ODD;
	}
	if(anim_state == ANIM_S_WAIT_ODD && check_timer(&anim_timer)) {
		// Waiting to light up ODD
		for(i=0; i < LEDS; i++) {
			u32 lidx = i*3;
//			if( (i % 2) == 0) {
				pixels[lidx]     = OFF_COLOR_G;
				pixels[lidx + 1] = OFF_COLOR_R;
				pixels[lidx + 2] = OFF_COLOR_B;
//			} else {
//				pixels[lidx]     = ON_COLOR_G;
//				pixels[lidx + 1] = ON_COLOR_R;
//				pixels[lidx + 2] = ON_COLOR_B;
//			}
		}
		switch_buffers();
		start_ms_timer(&anim_timer, 1000);
		anim_state = ANIM_S_WAIT_EVEN;
	}
}

//////////////////////////////////////////////////////////////////////////////////
// MAIN Setup & process
//////////////////////////////////////////////////////////////////////////////////
void setup() {
  // for delays - CP0Count counts at half the CPU rate
  Fcp0 = GetSystemClock() / 1000000 / 2;   // max = 40 for 80MHz

  // setup-buffers
  lw_buffer = pixel_buff_one;
  pixels = pixel_buff_two;
  
  //lb_setup();
  lw_setup();
  anim_setup();
}

void loop() {
  //lb_process();
  lw_process();
  anim_process();
}
