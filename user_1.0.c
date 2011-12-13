/*
 * LED-Strip SPI - non-blocking
 * Author: Erich Buri <erich.buri@gmail.com>
 */
#define DEBUG_MODE NODEBUG
#include <stdlib.h>
#include <typedef.h>
#include <system.c>
#include <delay.c>
#include <spi.c>


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

u8 lb_state;			// LED_Blinker state - to show non-blocking I/O
u32 lb_timer_start;		// LED_Blinker us-counter-start
u32 lb_timer_stop;		// LED_Blinker us-counter-stop
u32 lb_timer_delay;		// LED_Blinker ms-delay

// LED-Strip Writer variables
#define LW_S_WAIT_TO_WRITE_PIXEL 1
#define LW_S_WAIT_TO_WRITE_ZEROS  2
#define LW_ZEROS_NEEDED (3 * ((LEDS + 63) / 64))

u8 lw_state;			// This is the state of the LED_Writer State-machine
u8 * lw_buffer;			// Pointer to the buffer that the LED_Writer should draw
u32 lw_pixelIndex;      // current Pixel
u8 lw_zeroCounter;      // counter for zeros to send at the end

// Animator variables
#define ANIM_S_WAIT_EVEN 1
#define ANIM_S_WAIT_ODD  2
#define ON_COLOR_R 0x00
#define ON_COLOR_G 0x00
#define ON_COLOR_B 0xFF

#define OFF_COLOR_R 0x00
#define OFF_COLOR_G 0x00
#define OFF_COLOR_B 0x00

u8 * pixels;			// Pointer to the buffer were the Animator can work
u8  anim_state;			// Animator state - to show non-blocking I/O
u32 anim_timer_start;	// Animator us-counter-start
u32 anim_timer_stop;	// Animator us-counter-stop
u32 anim_timer_delay;	// Animator ms-delay


//////////////////////////////////////////////////////////////////////////////////
// LED Blinker
//////////////////////////////////////////////////////////////////////////////////
// setup	 CP0-based timer to 1ms
void lb_reset_us_counter() { lb_timer_start = GetCP0Count(); lb_timer_stop = lb_timer_start + 1000 * Fcp0; }

void lb_setup_counter(u32 delay) {
	// set ms-delay to <delay>
	lb_timer_delay = delay;
	lb_reset_us_counter();
}

void lb_setup() {
  // LED_Blinker setup
  pinmode(LB_LED_PIN,OUTPUT);
  lb_state = LB_S_WAIT_HIGH;
  lb_setup_counter(100);
}

// check counter us-delay is up, decrement ms-delay
// return 1, if time is up, 0, if timer still running
u8 lb_check_counter() {
	if(lb_timer_start < lb_timer_stop) {
		// "normal" case, just wait until GetCP0Count > lb_timer_stop
		if(GetCP0Count() > lb_timer_stop) {
			// us timer is up, reset
			lb_reset_us_counter();
			if( --lb_timer_delay <= 0 )
				return 1;
		}
	} else {
		// "overflow" case, wait until count is smaller then start 
		// (=> count did the overflow, too) and then until count > stop		
		if( GetCP0Count() < lb_timer_start && GetCP0Count() > lb_timer_stop ) {
			// us timer is up, reset
			lb_reset_us_counter();
			if( --lb_timer_delay <= 0 )
				return 1;
		}
	}
	return 0;
}

void lb_process() {
	if(lb_state == LB_S_WAIT_HIGH && lb_check_counter()) {
		// Waiting to go high, and timer is up
		digitalwrite(LB_LED_PIN, HIGH);
		lb_setup_counter(1000);
		lb_state = LB_S_WAIT_LOW;
	}
	if(lb_state == LB_S_WAIT_LOW && lb_check_counter()) {
		// Waiting to go high, and timer is up
		digitalwrite(LB_LED_PIN, LOW);
		lb_setup_counter(100);
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
	SPI_clock(GetSystemClock() / SPI_PBCLOCK_DIV16);
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
// setup CP0-based timer to 1ms
void anim_reset_us_counter() { anim_timer_start = GetCP0Count(); anim_timer_stop = anim_timer_start + 1000 * Fcp0; }

void anim_start_counter(u32 delay) {
	// set ms-delay to <delay>
	anim_timer_delay = delay;
	anim_reset_us_counter();
}

// check counter us-delay is up, decrement ms-delay
// return 1, if time is up, 0, if timer still running
u8 anim_check_counter() {
	if(anim_timer_start < anim_timer_stop) {
		// "normal" case, just wait until GetCP0Count > lb_timer_stop
		if(GetCP0Count() > anim_timer_stop) {
			// us timer is up, reset
			anim_reset_us_counter();
			if( --anim_timer_delay <= 0 )
				return 1;
		}
	} else {
		// "overflow" case, wait until count is smaller then start
		// (=> count did the overflow, too) and then until count > stop
		if( GetCP0Count() < anim_timer_start && GetCP0Count() > anim_timer_stop ) {
			// us timer is up, reset
			anim_reset_us_counter();
			if( --anim_timer_delay <= 0 )
				return 1;
		}
	}
	return 0;
}

void dataLink_setup() {
	anim_state = ANIM_S_WAIT_EVEN;
	anim_start_counter(100);
}

void dataLink_process() {
	u32 i;
	if(anim_state == ANIM_S_WAIT_EVEN && anim_check_counter()) {
		// Waiting to light up EVEN
		for(i=0; i < LEDS; i++) {
			u32 lidx = i*3;
			if( (i % 2) == 0) {
				pixels[lidx]     = ON_COLOR_G;
				pixels[lidx + 1] = ON_COLOR_R;
				pixels[lidx + 2] = ON_COLOR_B;
			} else {
				pixels[lidx]     = OFF_COLOR_G;
				pixels[lidx + 1] = OFF_COLOR_R;
				pixels[lidx + 2] = OFF_COLOR_B;
			}
		}
		anim_start_counter(1000);
		anim_state = ANIM_S_WAIT_ODD;
	}
	if(anim_state == ANIM_S_WAIT_ODD && anim_check_counter()) {
		// Waiting to light up ODD
		for(i=0; i < LEDS; i++) {
			u32 lidx = i*3;
			if( (i % 2) == 0) {
				pixels[lidx]     = OFF_COLOR_G;
				pixels[lidx + 1] = OFF_COLOR_R;
				pixels[lidx + 2] = OFF_COLOR_B;
			} else {
				pixels[lidx]     = ON_COLOR_G;
				pixels[lidx + 1] = ON_COLOR_R;
				pixels[lidx + 2] = ON_COLOR_B;
			}
		}
		anim_start_counter(500);
		anim_state = ANIM_S_WAIT_EVEN;
	}
}

//////////////////////////////////////////////////////////////////////////////////
// MAIN Setup & process
//////////////////////////////////////////////////////////////////////////////////
void setup() {
  // for delays - CP0Count counts at half the CPU rate
  Fcp0 = GetSystemClock() / 1000000 / 2;   // max = 40 for 80MHz

  // only using one buffer for the moment
  pixels = pixel_buff_one;
  lw_buffer = pixel_buff_one;
  
  //lb_setup();
  lw_setup();
  dataLink_setup();
}

void loop() {
  //lb_process();
  lw_process();
  dataLink_process();
}
