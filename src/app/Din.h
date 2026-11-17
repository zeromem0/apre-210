// AprecID's big lines in DIN 1451 Mittelschrift (DinFont.h, made by
// tools/dinfont.py): what the line is ("RS232 9600", "GND") and the line
// under it ("8N1  100% of 13"), in place of the built-in font at sizes 3 and
// 2. A Print, so that numbers and Frontend::printVolts() go through it as
// they went through the screen. The y given is where the built-in font's
// capitals began, so the lines keep their places; only the ink is drawn
// (the screen is cleared under it first), its edges between ink and ground.

#ifndef DIN_H
#define DIN_H

#include <Arduino.h>
#include "Lcd.h"

namespace Din {

enum Size : uint8_t { BIG, SMALL };

// how far a character moves the pen (for setting text right or in cells)
uint8_t advance(Size size, char c);

class Text : public Print {
public:
    Text(Lcd& tft, Size size, int16_t x, int16_t y);
    void color(uint16_t fg, uint16_t bg) { m_fg = fg; m_bg = bg; }
    void at(int16_t x, int16_t y);
    int16_t x() const { return m_x; }
    size_t write(uint8_t c) override;
private:
    Lcd&     m_tft;
    Size     m_size;
    int16_t  m_x, m_y;
    uint16_t m_fg, m_bg;
};

}
#endif
