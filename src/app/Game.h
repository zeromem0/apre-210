// Wave Raid: a side-scrolling game on the scope's screen. The land is a
// signal that goes by from right to left: DC, sine, pulse trains, UART
// frames, noise, triangle, ringing, PWM, its name and frequency on the top
// bar as a scope would say it. A ship flies over it: arrows move it, RUN
// fires straight ahead at what flies, OK drops bombs on what stands on the
// signal (chips, aerials, which fire back). Five ships; touching the land,
// a flyer or a shot loses one.
//
//   arrows   fly           RUN   fire (held: again and again)
//   OK       bomb          AUTO  autopilot (an arrow takes over); held: pause
//   MODE     back to the menu
//
// From the menu (Wave Raid), or at power-up with Settings, Startup "Game".

#ifndef GAME_H
#define GAME_H

#include "Lcd.h"

namespace Game {

void enter(Lcd& tft);
bool poll(Lcd& tft);            // false: back to the menu

}
#endif
