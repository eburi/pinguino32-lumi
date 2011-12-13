/*
  you find most available functions in p32/include/pinguino/core/
*/

#define DEBUG_MODE NODEBUG
#include <typedef.h>
#include <system.c>
#include <spi.c>
#include <delay.c>

#define WAIT_STATE 1
#define WRITE_HIGH 2
#define WRITE_LOW 3


u8 state;
u8 next_state;
u32 startTime,stopTime;
u8 Fcp0;
u32 delay;

void setup() {
    // initialize the digital pin as an output.
    // Pin 13 has an LED connected on most Arduino boards:
    pinmode(13, OUTPUT);
    // CP0Count counts at half the CPU rate
    Fcp0 = GetSystemClock() / 1000000 / 2;   // max = 40 for 80MHz
    state = WRITE_HIGH;
}

void loop() {

  switch (state){
    
    case WAIT_STATE:
      // wait till count reaches the stop value
      
      if(GetCP0Count() >= stopTime) {
        /*time elapsed*/
        if (--delay <= 0){
          state = next_state;
        } else {
          startTime = GetCP0Count();
          stopTime = startTime + 1000 * Fcp0;
        }
      }

      break;

    case WRITE_LOW:
      digitalwrite(13, LOW);
      startTime = GetCP0Count();
      stopTime = startTime + 1000 * Fcp0;
      delay=100;
      state=WAIT_STATE;
      next_state=WRITE_HIGH;
      break;
    case WRITE_HIGH:
      digitalwrite(13, HIGH);
      startTime = GetCP0Count();
      stopTime = startTime + 1000 * Fcp0;
      delay=1000;
      state=WAIT_STATE;
      next_state=WRITE_LOW;
      break;
  }
} 


