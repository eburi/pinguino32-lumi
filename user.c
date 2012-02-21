/*
 * LED-Strip SPI - non-blocking
 * Author: Erich Buri <erich.buri@gmail.com>
 *
 * - Uses different buffers for animator and LED-Strip-Writer
 * - Uses common functions for the timer-stuff
 * - Implements SPI-Emulation on multiple ports:
 *   - soft SPI 1: Pin 3 data, Pin 4 clock
 *   - soft SPI 2: Pin 5 data, Pin 6 clock
 * - Adds cdc to change data
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
#define L_WIDTH 32
#define L_HEIGHT 32
#define LEDS ( L_WIDTH * L_HEIGHT )
#define ZEROS_NEEDED (3 * ((LEDS + 63) / 64))

u8 Fcp0;				// number of GetCP0Count()'s for one microsecond
u8 pixel_buff_one[LEDS * 3];    // Two buffer's. One of them is currently drawn,
u8 pixel_buff_two[LEDS * 3];	// the other can be edited

// LED-Strip Writer variables
#define LW_S_WAIT_TO_WRITE_PIXEL 1
#define LW_S_WAIT_TO_WRITE_ZEROS  2

u8  lw_state;			// This is the state of the LED_Writer State-machine
u8 *lw_buffer;			// Pointer to the buffer that the LED_Writer should draw
u32 lw_pixelIndex;      // current Pixel
u8  lw_zeroCounter;      // counter for zeros to send at the end

u8 *pixels;				// Pointer to the buffer were the dataLink will buffer incoming data

// DataLink

//#define ROTATE_CW_90
#define ROTATE_CW_180

// Pixels on Lumi are oriented as GRB -> Pixels coming in are RGB
u8 colorOffsetMap[3] = { 1, 0, 2 };

u8  writeColByte;
u32 writeIndexX;
u32 writeIndexY;

#define DATA_LINK_TIMEOUT 5000
timerContext dataLink_timer;// Timer variables for the Animator

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
// LED-Strip Writer
//////////////////////////////////////////////////////////////////////////////////
void lw_setup() {
  // start by writing zeros to wake-up latch(s)
  lw_state = LW_S_WAIT_TO_WRITE_ZEROS;
  lw_pixelIndex = 0;
  lw_zeroCounter = ZEROS_NEEDED;

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
// DataLink
//////////////////////////////////////////////////////////////////////////////////
void dataLink_setup() {
  writeColByte = 0;
  writeIndexX = 0;
  writeIndexY = 0;

  CDCprintf("READY!\n");

  start_ms_timer(&dataLink_timer, DATA_LINK_TIMEOUT);
  // Nothing more to do here, CDC is initialised in main32.c
}

void dataLink_process() {
  char buffer[64];
  u8 bytesRead; // Will be max 64
  u8 i = 0;
  u32 insertPos = 0;

  if(check_timer(&dataLink_timer)) {
    CDCprintf("Timeout, init index - READY!\n");

    writeColByte = 0;
    writeIndexX = 0;
    writeIndexY = 0;

    start_ms_timer(&dataLink_timer, DATA_LINK_TIMEOUT);
  }

  bytesRead = CDCgets(buffer);

  if(bytesRead > 0) {
    // Reset Timer
    start_ms_timer(&dataLink_timer, DATA_LINK_TIMEOUT);

    // write bytes to buffer
    for(i = 0; i < bytesRead; i++) {

#if defined ROTATE_CW_90

      // rotation cw 90
      // width => height, height => width
      if( writeIndexX % 2 == 0 ) {
        // even column
        insertPos = ( (L_WIDTH - 1 - writeIndexY) + writeIndexX * L_HEIGHT );        
      } else {
        // odd column
        insertPos = ( writeIndexY + writeIndexX * L_HEIGHT );
      }

#elif defined ROTATE_CW_180

      // rotation cw 180
      // start from the bottom (x = WIDTH - 1 - x, y= HEIGHT - 1 - y)
      if( (L_HEIGHT - 1 - writeIndexY) % 2 == 0 ) {
        // even line
        insertPos = ( (L_HEIGHT - 1 - writeIndexY) * L_WIDTH ) + ( L_WIDTH - 1 - writeIndexX);
      } else {
        // odd line
        insertPos = ( (L_HEIGHT - 1 - writeIndexY) * L_WIDTH ) +  writeIndexX;
      }

#else
      // no rotation
      if( writeIndexY % 2 == 0 ) {
        // even line
        insertPos = ( writeIndexY * L_WIDTH ) + writeIndexX;
      } else {
        // odd line
        insertPos = ( writeIndexY * L_WIDTH ) + (L_WIDTH - 1 - writeIndexX);
      }

#endif

      insertPos *= 3;
      insertPos += colorOffsetMap[writeColByte];  

      /*DEBUG*///CDCprintf("x: %d, y: %d, byte: %d = %d\n", writeIndexX, writeIndexY, writeColByte, insertPos);

      pixels[insertPos] = buffer[i];

      // incerement counters
      writeColByte += 1;
      if(writeColByte == 3) {  
        // reset color-byte-counter
        writeColByte = 0;

        // got to next column
        writeIndexX += 1;
#ifdef ROTATE_CW_90
        if(writeIndexX == L_HEIGHT) {
#else
        if(writeIndexX == L_WIDTH) {
#endif
          // reset column
          writeIndexX = 0;
          // go to next row
          writeIndexY += 1;
        }
      }

#ifdef ROTATE_CW_90
      if(writeIndexY == L_WIDTH) {
#else
      if(writeIndexY == L_HEIGHT) { 
#endif
          // We're at the end of the buffer
          /*DEBUG*///CDCprintf("Received an image, switching buffers - READY!\n");
          switch_buffers();

          writeColByte = 0;
          writeIndexX = 0;
          writeIndexY = 0;
        }
      }
    }
  }

  //////////////////////////////////////////////////////////////////////////////////
  // MAIN Setup & process
  //////////////////////////////////////////////////////////////////////////////////
  void setup() {
    u32 i;

    CDCprintf("Setup..\n");

    // for delays - CP0Count counts at half the CPU rate
    Fcp0 = GetSystemClock() / 1000000 / 2;   // max = 40 for 80MHz

    // Reset pixels
    for(i = 0; i< LEDS*3; i++) {
      pixel_buff_one[i] = 0x00;
      pixel_buff_two[i] = 0x00;
    }

    // setup-buffers
    lw_buffer = pixel_buff_one;
    pixels = pixel_buff_two;


    lw_setup();
    dataLink_setup();
  }

  void loop() {
    lw_process();
    dataLink_process();
  }
