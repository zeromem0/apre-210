// Logger: over 60 seconds, two channels side by side: the multimeter's
// voltage (knob on V) or current (knob on mA or A) (Meter.h), every reading its
// chip sends (about five a second); and the DC voltage on the scope input
// (ScopeDc.h), five readings a second. Two views, AUTO to change:
//   60 s record   RUN starts a recording of 60 s, drawn from the left, the
//                 readings sent to the page ("log" messages)
//   live          the last 60 s, the newest at the right edge, the graph
//                 sliding left and scaled to what it shows; RUN holds it
// and four layouts, DOWN / UP: two graphs one over the other, both traces in
// one graph (the scope's scale on the right), the meter alone, the scope
// alone.
//
//   MODE        back to the menu (OK held too)

#ifndef LOGGER_H
#define LOGGER_H

#include "Lcd.h"

namespace Logger {

void enter(Lcd& tft);           // the meter powered, the screen drawn
bool poll(Lcd& tft);            // false: back to the menu
void leave();                   // the meter off

}
#endif
