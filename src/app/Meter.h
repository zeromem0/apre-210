// The DST-210's multimeter, as the FNIRSI firmware drives it (apre-atlas.md,
// "Multimetrul DST-210"): its isolated side powered by PB12, a chip on USART1
// at 9600 8N1 that is told the function in 10 bytes (AA 55 hi lo 0 0 0 0 0
// sum) and answers in 12 (5A A5 and what an LCD would show), the rotary switch
// read by the processor. For now a probe: turned on by the "meter" command,
// it sends each answer to the page as it came, decoded as far as is known.
// The chip is an SDIC SD7501; the 9 bytes after 5A A5 are its LCD memory.
#ifndef METER_H
#define METER_H

#include <stdint.h>

namespace Meter {

void begin();                   // power the meter, start USART1
void end();                     // and off again
bool on();
void poll();                    // every turn of the main loop
void command(uint16_t word);    // AA 55 hi lo ... to the chip
uint8_t position();             // the rotary switch, 1..10 (0: none read)

// The positions as FNIRSI numbers them (0x200025FF; on the case 7 and 8 the
// other way round): 1 AUto, 2 V, 3 ohm, 4 buzzer, 5 diode, 6 capacitance,
// 7 temperature, 8 Hz, 9 mA, 10 A.
const uint8_t POS_V = 2;
const uint8_t POS_MA = 9;
const uint8_t POS_A = 10;

// The last reading the chip sent: what its LCD shows as a number.
struct Value {
    uint32_t seq;       // counts the frames; 0: none since begin()
    uint32_t ms;        // when it came
    bool     num;       // the digits are a number (not "AUto", "0L", blank...)
    bool     ol;        // "0L": over the range
    int32_t  n;         // the digits as an integer, signed (19999 at most)
    uint8_t  dp;        // digits after the point
    int8_t   exp;       // the unit as a power of ten of V or A: mV/mA -3, uA -6, V/A 0
    char     unit[3];   // "mV", "V", "uA", "mA", "A"; empty where not known
};
const Value& value();

}

#endif
