/*
 * user.c
 *
 *  Created on: Nov 22, 2011
 *      Author: cede
 */

#include <stdlib.h>
#include "delay.c"
#include "spi.c"

#define BLACK 0x80
#define WHITE 0xff
#define STATE_COLOR 0x1
#define STATE_BLACK 0x2
#define LEDS 32 * 20

u8 * pixels;
u8   state;

void writezeros(u16 n) {

  while(n--)
    SPI_write(0);

}

void color() {
  u16 i;

  for(i=0; i < LEDS; i++) {

      if (i % 2 == 0) {
          pixels[3*i] = WHITE;
          pixels[3*i + 1] = BLACK;
          pixels[3*i + 2] = BLACK;
      } else {
          pixels[3*i] = BLACK;
          pixels[3*i + 1] = BLACK;
          pixels[3*i + 2] = WHITE;
      }

  }
}

void black() {

  memset(pixels,BLACK, LEDS * 3);

}

void setup() {

  pinmode(13,OUTPUT);
  /*all pixels black*/
  pixels = (u8 *) malloc( LEDS * 3);
  black();

  state = STATE_COLOR;

  SPI_init();
  /*10mhz - faster is no working*/
  SPI_clock(GetSystemClock() / SPI_PBCLOCK_DIV8);
  SPI_mode(SPI_MASTER);

  // Issue initial latch to 'wake up' strip (latch length varies w/numLEDs)
  writezeros(3 * ((LEDS + 63) / 64));

}

void show() {

  u8 data;
  u16 i;

  for(i=0; i < 3* LEDS; i++) {
      data = pixels[i];
      SPI_write(data);
  }

  // to 'latch' the data, we send just zeros
  //writezeros(3*numLEDs*2);
  //writezeros(4);
  // 20111028 pburgess: correct latch length varies --
  // three bytes per 64 LEDs.
  writezeros(3 * ((LEDS + 63) / 64));

  /*tune this*/
  Delayms(3);

}

void loop() {

  if (state == STATE_COLOR ) {
      color();
      state = STATE_BLACK;
  } else {
      black();
      state = STATE_COLOR;
  }

  show();
  //Delayms(20);

}


