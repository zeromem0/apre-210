// The scope input as a DC voltmeter (ScopeDc.h).
#include <Arduino.h>
#include "ScopeDc.h"
#include "Acquire.h"
#include "Frontend.h"

namespace ScopeDc {

static const uint32_t HZ        = 51200;
static const uint8_t  R_MIN     = 5;        // 500 mV a division, as AprecID starts
static const uint8_t  R_MAX     = 9;        // 10 V a division
static const uint32_t READ_MS   = 200;
static const uint32_t SETTLE_MS = 60;       // after a range change: the relay, the offset
static const int32_t  UP_AT     = 1500;     // counts from zero (of +-2048) past which the next range up is taken
static const int32_t  DOWN_AT   = 1200;     // and below which, on a more sensitive range, it is taken instead
static const uint16_t CLIP_LO   = 16, CLIP_HI = 4064;

static bool     g_on;
static uint8_t  g_range;
static uint32_t g_settle;                   // millis() until which the halves are thrown away
static uint32_t g_t0;
static int64_t  g_sum;
static uint32_t g_cnt;
static uint16_t g_lo, g_hi;
static bool     g_clip;
static Value    g_val;

static void restart(uint32_t now) {
    g_sum = 0; g_cnt = 0;
    g_lo = 0xFFFF; g_hi = 0;
    g_clip = false;
    g_t0 = now;
}

static void setRange(uint8_t r) {
    Acq::pause();
    Frontend::setRange(r);
    Acq::discard();
    Acq::resume();
    g_range = r;
    uint32_t now = millis();
    g_settle = now + SETTLE_MS;
    restart(now + SETTLE_MS);
}

void begin() {
    Acq::pause();
    Acq::onBlock(nullptr);                  // polled here
    Frontend::setCouplingDC(true);
    Acq::setSampleRate(HZ);
    g_val = Value();
    g_on = true;
    setRange(R_MAX);
}

void end() {
    if (!g_on) return;
    Acq::pause();
    g_on = false;
}

const Value& value() { return g_val; }
uint8_t range() { return g_range; }

void poll() {
    if (!g_on) return;
    uint16_t n;
    bool over;
    const uint16_t* s = Acq::readyHalf(n, over);
    if (!s) return;
    uint32_t now = millis();
    if ((int32_t)(now - g_settle) < 0) return;
    uint16_t lo = Acq::lastLo(), hi = Acq::lastHi();
    uint32_t sum = 0;                       // 1024 x 4080 at most
    for (uint16_t i = 0; i < n; i++) sum += s[i];
    if (!Acq::halfIntact()) return;         // overwritten while added up
    bool clip = lo <= CLIP_LO || hi >= CLIP_HI;
    if (clip && g_range < R_MAX) { setRange((uint8_t)(g_range + 1)); return; }

    g_sum += sum;
    g_cnt += n;
    if (lo < g_lo) g_lo = lo;
    if (hi > g_hi) g_hi = hi;
    g_clip |= clip;
    if (now - g_t0 < READ_MS || !g_cnt) return;

    int64_t zero = Frontend::zeroCounts(), cpv = Frontend::countsPerVolt();
    int64_t num = (g_sum - zero * (int64_t)g_cnt) * 1000000, den = cpv * (int64_t)g_cnt;
    g_val.uv = (int32_t)((num + (num < 0 ? -den / 2 : den / 2)) / den);
    g_val.range = g_range;
    g_val.clipped = g_clip;
    g_val.seq++;

    // the range for the next reading, from the extremes seen
    int32_t a = (int32_t)g_hi - (int32_t)zero, b = (int32_t)zero - (int32_t)g_lo;
    int32_t far = a > b ? a : b;                         // counts from zero on this range
    int64_t mv = (int64_t)far * 1000 / cpv;
    uint8_t want = g_range;
    if (far > UP_AT && g_range < R_MAX) want = (uint8_t)(g_range + 1);
    else
        for (uint8_t r = R_MIN; r < g_range; r++)
            if (mv * Frontend::countsPerVoltAt(r) / 1000 < DOWN_AT) { want = r; break; }
    if (want != g_range) setRange(want);
    else restart(now);
}

}  // namespace ScopeDc
