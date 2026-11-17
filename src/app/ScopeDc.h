// The scope input (the BNC, x1 probe) as a DC voltmeter, for the Logger: DC
// coupling, the converter polled at 51.2 kS/s (a half of 1024 samples is one
// period of 50 Hz mains, so the hum averages out), a reading every 200 ms as
// the mean of all the samples, the range picked by itself: the most sensitive
// from 500 mV a division (+-2.5 V) to 10 V a division (+-50 V) that holds
// what was seen, up at once when the converter clips.
//
// The caller (main.cpp) saves the range before begin() and puts the tester's
// block function, rate and range back after end().
#ifndef SCOPEDC_H
#define SCOPEDC_H

#include <stdint.h>

namespace ScopeDc {

struct Value {
    uint32_t seq;       // counts the readings; 0: none since begin()
    int32_t  uv;        // the mean, in microvolts at the probe tip
    uint8_t  range;     // the range it was read on (Frontend, 5..9)
    bool     clipped;   // the converter was at an end: the value is not the voltage
};

void begin();
void end();
void poll();            // every turn of the main loop
const Value& value();
uint8_t range();        // the range in force

}
#endif
