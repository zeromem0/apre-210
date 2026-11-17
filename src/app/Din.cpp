#include "Din.h"
#include "DinFont.h"

namespace Din {

static const DinFont& font(Size s) { return s == BIG ? DIN_BIG : DIN_SMALL; }

// a quarter, a half, three quarters of the way from the ground to the ink
static uint16_t mix(uint16_t bg, uint16_t fg, uint8_t l) {
    uint16_t r = (uint16_t)((((bg >> 11) & 31) * (3 - l) + ((fg >> 11) & 31) * l) / 3);
    uint16_t g = (uint16_t)((((bg >> 5) & 63) * (3 - l) + ((fg >> 5) & 63) * l) / 3);
    uint16_t b = (uint16_t)(((bg & 31) * (3 - l) + (fg & 31) * l) / 3);
    return (uint16_t)(r << 11 | g << 5 | b);
}

uint8_t advance(Size size, char c) {
    if (c < 32 || c > 126) c = '?';
    return font(size).g[c - 32].adv;
}

Text::Text(Lcd& tft, Size size, int16_t x, int16_t y)
    : m_tft(tft), m_size(size), m_x(x), m_y(y), m_fg(0xFFFF), m_bg(0) {}

void Text::at(int16_t x, int16_t y) { m_x = x; m_y = y; }

size_t Text::write(uint8_t c) {
    const DinFont& f = font(m_size);
    if (c < 32 || c > 126) c = '?';
    const DinGlyph& g = f.g[c - 32];
    uint16_t col[4] = { m_bg, mix(m_bg, m_fg, 1), mix(m_bg, m_fg, 2), m_fg };
    int16_t y0 = (int16_t)(m_y - f.cap);
    uint32_t k = 0;
    for (uint8_t y = 0; y < f.h; y++) {
        // a row in runs of one level; the ground (level 0) is left as it is
        int16_t runX = 0;
        uint8_t runL = 0, runN = 0;
        for (uint8_t x = 0; x <= g.w; x++) {
            uint8_t l = 0;
            if (x < g.w) { l = (f.bits[g.off + (k >> 2)] >> (2 * (k & 3))) & 3; k++; }
            if (x < g.w && l == runL && runN) { runN++; continue; }
            if (runN && runL) m_tft.fillRect((int16_t)(m_x + g.x + runX), (int16_t)(y0 + y), runN, 1, col[runL]);
            runX = x; runL = l; runN = 1;
        }
    }
    m_x = (int16_t)(m_x + g.adv);
    return 1;
}

}
