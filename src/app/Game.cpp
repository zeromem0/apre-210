// Wave Raid (Game.h). No frame buffer (the RAM has no room for 150 kB): the
// playfield is built one column at a time in a small buffer, the sky, the
// signal, then everything over it, and each column is sent whole
// (Lcd::pushColumn), about 70 000 pixels a frame, a few milliseconds.
// Positions in sixteenths of a pixel; the land in world columns, kept in a
// ring of 512 ahead of and under the screen.
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include "Game.h"
#include "Theme.h"
#include "Keys.h"
#include "Beep.h"
#include "Din.h"
#include "Acquire.h"

namespace Game {

static const int16_t  W = 320, TOP = 18, H = 240 - TOP;   // the playfield, under the bar
static const uint32_t FRAME_MS = 33;
static const uint16_t RING = 512;
static const int16_t  Y_HI = 100, Y_LO = 228;             // where the land's top may be
static const int16_t  SHIP_W = 20, SHIP_H = 9;

// -- colours, from the theme ---------------------------------------------

enum { C_SKY, C_GRID, C_TRACE, C_GROUND, C_TEXT, C_MARK, C_ERR, C_OK, C_MUTED, NC };
static uint16_t P[NC];

static uint16_t mix(uint16_t a, uint16_t b, uint8_t q) {      // q quarters of b
    uint16_t r = (uint16_t)((((a >> 11) & 31) * (4 - q) + ((b >> 11) & 31) * q) / 4);
    uint16_t g = (uint16_t)((((a >> 5) & 63) * (4 - q) + ((b >> 5) & 63) * q) / 4);
    uint16_t c = (uint16_t)(((a & 31) * (4 - q) + (b & 31) * q) / 4);
    return (uint16_t)(r << 11 | g << 5 | c);
}

static void colours() {
    P[C_SKY] = TH.bg;       P[C_GRID] = TH.line;   P[C_TRACE] = TH.trace;
    P[C_GROUND] = mix(TH.bg, TH.trace, 1);
    P[C_TEXT] = TH.text;    P[C_MARK] = TH.mark;   P[C_ERR] = TH.err;
    P[C_OK] = TH.ok;        P[C_MUTED] = TH.muted;
}

// -- the pictures: '.' clear, 1 text, 2 trace, 3 err, 4 mark, 5 ok -------

struct Sprite { uint8_t w, h; const char* const* rows; };

static const char* const SHIP_ROWS[SHIP_H] = {
    "11..................",
    ".111................",
    "..1111.....222......",
    "3.11111111122221....",
    "33111111111111111111",
    "3.111111111111111...",
    "..11111111111.......",
    ".1111...............",
    "111.................",
};
static const char* const GLITCH_ROWS[10] = {
    ".....33.....",
    "....3443....",
    "..33444433..",
    ".3444444443.",
    "344445544443",
    "344445544443",
    ".3444444443.",
    "..33444433..",
    "....3443....",
    ".....33.....",
};
static const char* const DART_ROWS[7] = {
    "....3.......",
    "...33.......",
    "..444444444.",
    ".44444444441",
    "..444444444.",
    "...33.......",
    "....3.......",
};
static const char* const CHIP_ROWS[7] = {
    "1111111111111111",
    "1444444444444441",
    "14.4444444444441",
    "1444444444444441",
    "1111111111111111",
    ".1.1.1.1.1.1.1.1",
    ".1.1.1.1.1.1.1.1",
};
static const char* const MAST_ROWS[14] = {
    "....3....",
    "...333...",
    "....1....",
    "..1.1.1..",
    "...111...",
    "....1....",
    "....1....",
    "...1.1...",
    "...1.1...",
    "..1...1..",
    "..1...1..",
    ".1.....1.",
    ".1111111.",
    "111111111",
};
static const Sprite SHIP   = { SHIP_W, SHIP_H, SHIP_ROWS };
static const Sprite GLITCH = { 12, 10, GLITCH_ROWS };
static const Sprite DART   = { 12, 7, DART_ROWS };
static const Sprite CHIP   = { 16, 7, CHIP_ROWS };
static const Sprite MAST   = { 9, 14, MAST_ROWS };

static uint16_t inkOf(char c) {
    switch (c) {
        case '1': return P[C_TEXT];
        case '2': return P[C_TRACE];
        case '3': return P[C_ERR];
        case '4': return P[C_MARK];
        default:  return P[C_OK];
    }
}

// -- random, sine --------------------------------------------------------

static uint32_t g_rnd = 0x2545F491;
static uint32_t rnd() { g_rnd ^= g_rnd << 13; g_rnd ^= g_rnd >> 17; g_rnd ^= g_rnd << 5; return g_rnd; }
static int32_t  rndIn(int32_t lo, int32_t hi) { return lo + (int32_t)(rnd() % (uint32_t)(hi - lo + 1)); }

static const int8_t QSIN[65] = {
    0, 3, 6, 9, 12, 16, 19, 22, 25, 28, 31, 34, 37, 40, 43, 46, 49, 51, 54, 57, 60, 63, 65, 68, 71, 73,
    76, 78, 81, 83, 85, 88, 90, 92, 94, 96, 98, 100, 102, 104, 106, 107, 109, 111, 112, 113, 115, 116,
    117, 118, 120, 121, 122, 122, 123, 124, 125, 125, 126, 126, 126, 127, 127, 127, 127 };
static int16_t isin(uint8_t a) {                               // a: 256 to the turn; -127..127
    uint8_t q = a & 63;
    switch (a >> 6) {
        case 0:  return QSIN[q];
        case 1:  return QSIN[64 - q];
        case 2:  return -QSIN[q];
        default: return -QSIN[64 - q];
    }
}

// -- the land ------------------------------------------------------------

enum { S_DC, S_SINE, S_PULSE, S_UART, S_NOISE, S_TRI, S_RING, S_PWM, S_N };

static uint8_t  g_land[RING];          // the top of each column (y <= 228 fits a byte)
static uint32_t g_scroll;             // the world column at the screen's left edge
static uint32_t g_gen;                // the next world column to make
static uint8_t  sType;
static int32_t  sLeft, sT, sC, sA, sPer, sDuty, sV, sAmp16;
static int16_t  sHiY, sLoY;
static uint16_t sFrame;               // UART: the bits still to go, lowest first
static uint8_t  sBitLeft, sBitW, sChar;
static int32_t  sPause;
static int16_t  g_prevY = 200, g_run;
static uint32_t g_lastTarget;

// the labels of the stretches, by where they begin, for the bar
struct Label { uint32_t at; char s[20]; };
static Label    g_lab[4];
static uint8_t  g_labN;

static const char MSG[] = "WAVE RAID  ";

// 1 px of land is 1/96000 s: 10 px a bit is 9600 baud, 96 px a period 1 kHz
static void hz(char* b, size_t n, const char* name, int32_t per) {
    int32_t f = 96000 / (per > 0 ? per : 1);
    if (f >= 1000) snprintf(b, n, "%s %ld.%ld kHz", name, (long)(f / 1000), (long)(f % 1000 / 100));
    else           snprintf(b, n, "%s %ld Hz", name, (long)f);
}

static void newStretch() {
    uint32_t d = g_scroll / 3000;                              // harder as it goes
    if (d > 8) d = 8;
    uint8_t t = g_gen < 400 ? S_DC : (uint8_t)(rnd() % S_N);
    sType = t;
    sT = 0;
    sLeft = rndIn(320, 700);
    sA = rndIn(24, 44) + (int32_t)d * 9;                       // the swing, in px
    if (sA > Y_LO - Y_HI - 8) sA = Y_LO - Y_HI - 8;
    sC = rndIn(Y_HI + sA / 2 + 4, Y_LO - sA / 2);              // the middle
    sHiY = (int16_t)(sC - sA / 2); sLoY = (int16_t)(sC + sA / 2);
    Label& L = g_lab[g_labN++ & 3];
    L.at = g_gen;
    switch (t) {
        case S_DC: {
            sC = rndIn(170, Y_LO - 6);
            long mv = (long)(Y_LO + 2 - sC) * 50;
            snprintf(L.s, sizeof(L.s), "DC %ld.%ld V", mv / 1000, mv % 1000 / 100);
            sLeft = rndIn(240, 420);
            break;
        }
        case S_SINE:  sPer = rndIn(70, 200); hz(L.s, sizeof(L.s), "SINE", sPer); break;
        case S_TRI:   sPer = rndIn(60, 180); hz(L.s, sizeof(L.s), "TRIANGLE", sPer); break;
        case S_PULSE:
            sPer = rndIn(48, 140); sDuty = rndIn(30, 70);
            hz(L.s, sizeof(L.s), "PULSE", sPer);
            snprintf(L.s + strlen(L.s), sizeof(L.s) - strlen(L.s), " %ld%%", (long)sDuty);
            break;
        case S_PWM:
            sPer = rndIn(60, 110);
            hz(L.s, sizeof(L.s), "PWM", sPer);
            break;
        case S_UART:
            sBitW = (uint8_t)(rnd() & 1 ? 10 : 20);
            snprintf(L.s, sizeof(L.s), "UART %u 8N1", sBitW == 10 ? 9600u : 4800u);
            sFrame = 0; sBitLeft = 0; sPause = 0;
            break;
        case S_NOISE: sV = sC << 4; strcpy(L.s, "NOISE"); break;
        case S_RING:
            sPer = rndIn(26, 44); sAmp16 = (sA / 2) << 4;
            strcpy(L.s, "RINGING");
            break;
    }
}

static int16_t landOne() {
    if (sLeft <= 0) newStretch();
    sLeft--;
    int32_t t = sT++, y = sC;
    switch (sType) {
        case S_DC: y = sC; break;
        case S_SINE: y = sC - isin((uint8_t)(t * 256 / sPer)) * (sA / 2) / 127; break;
        case S_TRI: {
            int32_t p = t % sPer, half = sPer / 2;
            y = p < half ? sLoY - (sA * p) / half : sHiY + (sA * (p - half)) / (sPer - half);
            break;
        }
        case S_PULSE: y = (t % sPer) * 100 < sDuty * sPer ? sHiY : sLoY; break;
        case S_PWM: {
            int32_t duty = 10 + 80 * t / (sLeft + t + 1);      // 10 % up to 90 % over the stretch
            y = (t % sPer) * 100 < duty * sPer ? sHiY : sLoY;
            break;
        }
        case S_UART:
            if (sPause) { sPause--; y = sHiY; break; }         // idle: high
            if (!sBitLeft) {
                if (!sFrame) {
                    if (rnd() % 5 == 0) { sPause = rndIn(40, 90); y = sHiY; break; }
                    char c = MSG[sChar++ % (sizeof(MSG) - 1)];
                    sFrame = (uint16_t)(0x200 | (uint8_t)c << 1);   // start 0, 8 bits, stop 1
                }
                sBitLeft = sBitW;
            }
            y = (sFrame & 1) ? sHiY : sLoY;
            if (--sBitLeft == 0) sFrame >>= 1;
            break;
        case S_NOISE:
            sV += rndIn(-40, 40);
            if (sV < (sHiY << 4)) sV = sHiY << 4;
            if (sV > (sLoY << 4)) sV = sLoY << 4;
            y = sV >> 4;
            break;
        case S_RING:
            if (t < 30) { y = sLoY; break; }                  // low, then the step up, ringing
            y = sHiY + 12 - isin((uint8_t)((t - 30) * 256 / sPer)) * (sAmp16 >> 4) / 127;
            sAmp16 = sAmp16 * 252 / 256;
            break;
    }
    if (y < Y_HI) y = Y_HI;
    if (y > Y_LO) y = Y_LO;
    return (int16_t)y;
}

// -- what is about -------------------------------------------------------

struct Target { uint32_t wx; int16_t y; uint8_t kind; bool on; uint16_t fire; };   // standing on the land
struct Thing  { int32_t x, y, vx, vy; uint8_t kind; bool on; uint16_t t; int32_t base; };
struct Boom   { int32_t x; int16_t y; uint8_t age, size; bool ground; bool on; };

enum { T_CHIP, T_MAST };
enum { E_GLITCH, E_DART };

static Target g_tg[10];
static Thing  g_shot[5], g_bomb[2], g_foe[6], g_hit[8];          // ours, bombs, flyers, theirs
static Boom   g_boom[10];

static void placeTarget(uint32_t wx, int16_t y) {
    for (Target& t : g_tg) if (!t.on) {
        t.wx = wx; t.y = y; t.kind = (uint8_t)(rnd() % 3 == 0 ? T_MAST : T_CHIP);
        t.on = true; t.fire = (uint16_t)rndIn(30, 90);
        g_lastTarget = wx;
        return;
    }
}

static void makeLand() {
    while (g_gen < g_scroll + W + 64) {
        int16_t y = landOne();
        g_land[g_gen & (RING - 1)] = y;
        g_run = y == g_prevY ? (int16_t)(g_run + 1) : 0;
        g_prevY = y;
        if (g_run == 20 && g_gen - g_lastTarget > 150 && g_gen > 500 && rnd() % 2 == 0)
            placeTarget(g_gen - 19, y);
        g_gen++;
    }
}

static int16_t landAt(int32_t sx) {                           // the land's top at screen x
    if (sx < 0) sx = 0;
    if (sx > W - 1) sx = W - 1;
    return g_land[(g_scroll + (uint32_t)sx) & (RING - 1)];
}

static void boom(int32_t x, int16_t y, uint8_t size, bool ground) {
    for (Boom& b : g_boom) if (!b.on) {
        b.x = ground ? (int32_t)(g_scroll + x) : x; b.y = y; b.age = 0; b.size = size;
        b.ground = ground; b.on = true;
        return;
    }
}

// -- the game ------------------------------------------------------------

enum { TITLE, PLAY, DEAD, OVER, PAUSE };
static uint8_t  g_state;
static int32_t  g_px, g_py;           // the ship, in 1/16 px
static uint8_t  g_lives;
static uint32_t g_score, g_hi;
static uint32_t g_frame, g_lastMs, g_deadAt;
static uint16_t g_safe;               // frames the ship cannot be hit (after coming back)
static bool     g_auto, g_shownAuto = true;   // the autopilot (AUTO)
static uint16_t g_cool, g_foeIn;
static uint16_t g_sub;                // the scroll's sixteenths
static uint32_t g_shownScore = 0xFFFFFFFF, g_shownHi = 0xFFFFFFFF;
static uint8_t  g_shownLives = 0xFF;
static const char* g_shownLab = nullptr;
static uint16_t g_col[H];

static uint16_t speed16() { uint32_t d = g_scroll / 3000; return (uint16_t)(32 + (d > 8 ? 8 : d) * 4); }   // 2 px a frame and up

static void reset() {
    memset(g_tg, 0, sizeof(g_tg)); memset(g_shot, 0, sizeof(g_shot)); memset(g_bomb, 0, sizeof(g_bomb));
    memset(g_foe, 0, sizeof(g_foe)); memset(g_hit, 0, sizeof(g_hit)); memset(g_boom, 0, sizeof(g_boom));
    g_scroll = 0; g_gen = 0; g_sub = 0; sLeft = 0; g_labN = 0; g_run = 0; g_lastTarget = 0;
    g_rnd ^= millis() * 2654435761u;
    if (!g_rnd) g_rnd = 1;
    makeLand();
    g_px = 40 << 4; g_py = 60 << 4;
    g_lives = 5; g_score = 0; g_shownAuto = !g_auto; g_safe = 60; g_cool = 0; g_foeIn = 90;
    g_shownScore = 0xFFFFFFFF; g_shownLives = 0xFF; g_shownLab = nullptr; g_shownHi = 0xFFFFFFFF;
}

static void bar(Lcd& tft, bool all) {
    tft.setTextSize(1);
    if (all) tft.fillRect(0, 0, W, TOP, TH.bar);
    tft.setTextColor(TH.barText, TH.bar);
    if (all || g_score != g_shownScore) {
        char b[16]; snprintf(b, sizeof(b), "SCORE %06lu", (unsigned long)g_score);
        tft.setCursor(2, 5); tft.print(b); g_shownScore = g_score;
    }
    if (all || g_hi != g_shownHi) {
        char b[16]; snprintf(b, sizeof(b), "HI %06lu", (unsigned long)g_hi);
        tft.setCursor(80, 5); tft.print(b); g_shownHi = g_hi;
    }
    if (all || g_lives != g_shownLives) {
        tft.fillRect(138, 2, 52, 14, TH.bar);
        for (uint8_t i = 0; i < g_lives && i < 5; i++) {      // a small ship for each one left
            int16_t x = (int16_t)(140 + i * 10);
            tft.fillRect(x, 8, 9, 2, TH.barHi);
            tft.fillRect(x, 6, 2, 2, TH.barHi);
            tft.fillRect(x + 5, 7, 3, 1, TH.barHi);
        }
        g_shownLives = g_lives;
    }
    if (all || g_auto != g_shownAuto) {                       // the autopilot flying
        tft.fillRect(192, 2, 18, 14, TH.bar);
        if (g_auto) { tft.setTextColor(TH.ok, TH.bar); tft.setCursor(194, 5); tft.print("AP"); }
        g_shownAuto = g_auto;
    }
    // the stretch the ship is over, as a scope would name it
    uint32_t at = g_scroll + (uint32_t)(g_px >> 4);
    const char* lab = g_lab[0].s;
    uint32_t best = 0;
    for (uint8_t i = 0; i < 4 && i < g_labN; i++)
        if (g_lab[i].at <= at && g_lab[i].at >= best) { best = g_lab[i].at; lab = g_lab[i].s; }
    if (g_state == PAUSE) lab = "PAUSED";
    if (all || lab != g_shownLab) {
        tft.fillRect(212, 2, W - 212, 14, TH.bar);
        tft.setTextColor(g_state == PAUSE ? TH.err : TH.barHi, TH.bar);
        tft.setCursor(214, 5);
        tft.print(lab);
        g_shownLab = lab;
    }
}

static bool overlap(int32_t ax, int32_t ay, int32_t aw, int32_t ah, int32_t bx, int32_t by, int32_t bw, int32_t bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

static void lose() {
    boom(g_px >> 4, (int16_t)((g_py >> 4) + 4), 14, false);
    boom((g_px >> 4) + 10, (int16_t)((g_py >> 4) + 2), 10, false);
    Beep::tone(110, 400);
    g_state = DEAD;
    g_deadAt = g_frame;
}

static void fireAt(int32_t x, int32_t y, int32_t vx, int32_t vy) {
    for (Thing& h : g_hit) if (!h.on) { h.on = true; h.x = x; h.y = y; h.vx = vx; h.vy = vy; return; }
}

// The autopilot (AUTO): keeps the ship 25 px over the highest land of the
// next stretch, gets out of the way of what flies at its height, fires at
// what is in line ahead, and drops a bomb when the fall (t frames: t(t+1)/8 px)
// and the drift (20/16 px a frame, slowing) bring it onto a target.
static int32_t isqrt(int32_t n) { int32_t r = 0; while ((r + 1) * (r + 1) <= n) r++; return r; }

static void pilot(bool& up, bool& dn, bool& lt, bool& rt, bool& fire, bool& bomb, uint16_t sp) {
    int32_t sx = g_px >> 4, sy = g_py >> 4, mid = sy + SHIP_H / 2;
    int16_t high = Y_LO;
    for (int32_t x = sx - 4; x < sx + SHIP_W + 80; x += 2) { int16_t l = landAt(x); if (l < high) high = l; }
    int32_t lowest = high - SHIP_H - 25, want = lowest;
    if (want < TOP + 4) want = TOP + 4;
    for (const Thing& f : g_foe) if (f.on) {                  // what flies, near and in line
        int32_t fx = f.x >> 4, fm = (f.y >> 4) + 4;
        if (fx > sx - 8 && fx < sx + 120 && fm > mid - 18 && fm < mid + 18) {
            if (fx < sx + SHIP_W + 40 || f.kind == E_DART) want = fm > mid ? fm - 30 : fm + 26;
        }
    }
    for (const Thing& h : g_hit) if (h.on) {                  // their shots coming
        int32_t hx = h.x >> 4, hm = h.y >> 4;
        if (hx > sx - 4 && hx < sx + SHIP_W + 50 && hm > mid - 12 && hm < mid + 12) want = hm > mid ? hm - 22 : hm + 18;
    }
    if (want > lowest) want = lowest;
    if (want < TOP + 2) want = TOP + 2;
    up = want < sy - 1; dn = want > sy + 1;
    lt = sx > 44; rt = sx < 36;
    fire = false;
    for (const Thing& f : g_foe) if (f.on) {
        const Sprite& s = f.kind == E_DART ? DART : GLITCH;
        int32_t fx = f.x >> 4, fy = f.y >> 4;
        if (fx > sx + SHIP_W && fx < W && sy + 4 >= fy - 1 && sy + 4 <= fy + s.h) fire = true;
    }
    bomb = false;
    int32_t bx = sx + 8, by = sy + 8;
    for (const Target& t : g_tg) if (t.on) {
        const Sprite& s = t.kind == T_MAST ? MAST : CHIP;
        int32_t tc = (int32_t)(t.wx - g_scroll) + s.w / 2, d = t.y - (by + 2);
        if (d <= 0 || tc < bx) continue;
        int32_t f = isqrt(8 * d), drift = 0;
        for (int32_t k = 0, v = 20; k < f && v > 0; k++, v--) drift += v;
        int32_t need = (drift + (int32_t)sp * f) / 16;          // how far ahead it must be now
        int32_t off = tc - bx - need;
        if (off > -4 && off < 4) bomb = true;
    }
}

static void step() {
    g_frame++;
    uint16_t sp = speed16();
    g_sub = (uint16_t)(g_sub + sp);
    uint16_t cols = g_sub >> 4;                               // world columns gone by this frame
    g_sub &= 15;
    g_scroll += cols;
    static uint16_t dist;
    if (g_state == PLAY && (dist = (uint16_t)(dist + cols)) >= 16) { dist -= 16; g_score++; }   // a point each 16 columns flown
    makeLand();

    bool alive = g_state == PLAY;
    if (alive) {                                              // the ship
        const int32_t v = 40;
        bool up = Keys::down(Keys::UP), dn = Keys::down(Keys::DOWN);
        bool lt = Keys::down(Keys::LEFT), rt = Keys::down(Keys::RIGHT);
        if (g_auto && (up || dn || lt || rt)) g_auto = false;  // an arrow takes the stick back
        bool fire = Keys::down(Keys::RUN), bomb = Keys::hit(Keys::OK);
        if (g_auto) pilot(up, dn, lt, rt, fire, bomb, sp);
        if (up) g_py -= v;
        if (dn) g_py += v;
        if (lt) g_px -= v;
        if (rt) g_px += v;
        if (g_px < (2 << 4)) g_px = 2 << 4;
        if (g_px > ((W - SHIP_W - 60) << 4)) g_px = (W - SHIP_W - 60) << 4;
        if (g_py < ((TOP + 2) << 4)) g_py = (TOP + 2) << 4;
        if (g_py > ((240 - SHIP_H) << 4)) g_py = (240 - SHIP_H) << 4;
        if (g_cool) g_cool--;
        if (fire && !g_cool) {
            for (Thing& s : g_shot) if (!s.on) {
                s.on = true; s.x = g_px + ((SHIP_W - 2) << 4); s.y = g_py + (4 << 4); s.vx = 9 << 4;
                g_cool = 6;
                Beep::tone(1800, 8);
                break;
            }
        }
        if (bomb) {
            for (Thing& b : g_bomb) if (!b.on) {
                b.on = true; b.x = g_px + (8 << 4); b.y = g_py + (8 << 4); b.vx = 20; b.vy = 0;
                Beep::tone(500, 25);
                break;
            }
        }
        if (g_safe) g_safe--;
    }

    for (Thing& s : g_shot) if (s.on) {                       // our shots: straight on
        s.x += s.vx;
        int32_t x = s.x >> 4, y = s.y >> 4;
        if (x >= W || y >= landAt(x)) s.on = false;
    }
    for (Thing& b : g_bomb) if (b.on) {                       // bombs: falling, a little forward
        b.vy += 4; b.x += b.vx; b.y += b.vy;
        if (b.vx > 0) b.vx--;
        int32_t x = b.x >> 4, y = b.y >> 4;
        if (x < 0 || x >= W) { b.on = false; continue; }
        if (y + 2 >= landAt(x)) {
            b.on = false;
            bool hitOne = false;
            for (Target& t : g_tg) if (t.on) {
                int32_t tx = (int32_t)(t.wx - g_scroll);
                const Sprite& sp2 = t.kind == T_MAST ? MAST : CHIP;
                if (x + 3 >= tx && x - 3 <= tx + sp2.w) {
                    t.on = false; hitOne = true;
                    g_score += t.kind == T_MAST ? 150 : 100;
                    boom(tx + sp2.w / 2, (int16_t)(t.y - sp2.h / 2), 16, true);
                }
            }
            boom(x, landAt(x), hitOne ? 8 : 6, true);
            Beep::tone(hitOne ? 220 : 160, hitOne ? 140 : 60);
        }
    }

    // flyers come in from the right
    if (g_foeIn) g_foeIn--;
    else {
        uint32_t d = g_scroll / 3000; if (d > 8) d = 8;
        g_foeIn = (uint16_t)rndIn(40, 100 - (int32_t)d * 6);
        for (Thing& f : g_foe) if (!f.on) {
            int16_t low = Y_LO;
            for (int16_t x = W - 60; x < W; x += 4) if (landAt(x) < low) low = landAt(x);
            int32_t top = TOP + 8, bottom = low - 24;
            if (bottom <= top + 4) break;
            f.on = true; f.kind = (uint8_t)(rnd() % 3 == 0 ? E_DART : E_GLITCH);
            f.x = (W + 4) << 4;
            f.base = rndIn(top, bottom) << 4; f.y = f.base;
            f.vx = -(int32_t)sp - (f.kind == E_DART ? 40 : 12);
            f.t = (uint16_t)rndIn(0, 255);
            f.vy = rndIn(40, 90);                             // frames to its first shot
            break;
        }
    }
    for (Thing& f : g_foe) if (f.on) {
        f.x += f.vx;
        f.t++;
        if (f.kind == E_GLITCH) f.y = f.base + isin((uint8_t)(f.t * 4)) * 14 * 16 / 127;
        if ((f.x >> 4) < -16) { f.on = false; continue; }
        if (--f.vy <= 0 && (f.x >> 4) < W - 20 && (f.x >> 4) > (g_px >> 4) + 30) {   // a shot at the ship
            f.vy = rndIn(60, 120);
            int32_t dx = (g_px - f.x), dy = (g_py - f.y);
            int32_t ax = dx < 0 ? -dx : dx;
            fireAt(f.x, f.y + (4 << 4), -(int32_t)sp - 40, ax ? dy * 56 / ax : 0);
        }
        const Sprite& s = f.kind == E_DART ? DART : GLITCH;
        for (Thing& sh : g_shot) if (sh.on && overlap(sh.x >> 4, sh.y >> 4, 5, 2, f.x >> 4, f.y >> 4, s.w, s.h)) {
            sh.on = false; f.on = false;
            g_score += f.kind == E_DART ? 80 : 50;
            boom((f.x >> 4) + s.w / 2, (int16_t)((f.y >> 4) + s.h / 2), 10, false);
            Beep::tone(330, 70);
            break;
        }
    }
    // aerials fire up at what goes over
    for (Target& t : g_tg) if (t.on) {
        int32_t tx = (int32_t)(t.wx - g_scroll);
        if (tx < -20) { t.on = false; continue; }
        if (t.kind == T_MAST && tx < W - 10 && tx > (g_px >> 4) && --t.fire == 0) {
            t.fire = (uint16_t)rndIn(50, 100);
            fireAt((int32_t)(tx + 4) << 4, (int32_t)(t.y - 15) << 4, -(int32_t)sp - 8, -44);
        }
    }
    for (Thing& h : g_hit) if (h.on) {
        h.x += h.vx; h.y += h.vy;
        int32_t x = h.x >> 4, y = h.y >> 4;
        if (x < 0 || x >= W || y < TOP || y >= landAt(x)) h.on = false;
    }
    for (Boom& b : g_boom) if (b.on && ++b.age > 18) b.on = false;

    if (alive && !g_safe) {                                   // what the ship touched
        int32_t sx = g_px >> 4, sy = g_py >> 4;
        bool hit = false;
        for (int32_t x = sx + 1; x < sx + SHIP_W - 1 && !hit; x += 2) if (sy + SHIP_H - 2 >= landAt(x)) hit = true;
        for (Thing& f : g_foe) if (f.on && !hit) {
            const Sprite& s = f.kind == E_DART ? DART : GLITCH;
            if (overlap(sx + 2, sy + 2, SHIP_W - 4, SHIP_H - 4, f.x >> 4, f.y >> 4, s.w, s.h)) { hit = true; f.on = false; }
        }
        for (Thing& h : g_hit) if (h.on && !hit && overlap(sx + 2, sy + 2, SHIP_W - 4, SHIP_H - 4, h.x >> 4, h.y >> 4, 3, 3)) {
            hit = true; h.on = false;
        }
        if (hit) lose();
    }
    if (g_state == DEAD && g_frame - g_deadAt > 50) {
        if (g_lives > 1) {
            g_lives--;
            g_state = PLAY;
            g_px = 40 << 4;
            int16_t low = Y_LO;
            for (int16_t x = 30; x < 80; x++) if (landAt(x) < low) low = landAt(x);
            g_py = (int32_t)(low - 50 > TOP + 4 ? low - 50 : TOP + 4) << 4;
            g_safe = 75;
        } else {
            g_lives = 0;
            g_state = OVER;
        }
    }
    if (g_score > g_hi) g_hi = g_score;
}

// -- drawing -------------------------------------------------------------

static void spriteCol(const Sprite& s, int32_t sx, int32_t sy, int16_t x) {
    int32_t cx = x - sx;
    if (cx < 0 || cx >= s.w) return;
    for (uint8_t r = 0; r < s.h; r++) {
        char c = s.rows[r][cx];
        if (c == '.') continue;
        int32_t y = sy + r - TOP;
        if (y >= 0 && y < H) g_col[y] = inkOf(c);
    }
}

static void boxCol(int32_t bx, int32_t by, int16_t bw, int16_t bh, int16_t x, uint16_t c) {
    if (x < bx || x >= bx + bw) return;
    for (int16_t r = 0; r < bh; r++) {
        int32_t y = by + r - TOP;
        if (y >= 0 && y < H) g_col[y] = c;
    }
}

static void render(Lcd& tft) {
    bool showShip = g_state == PLAY && !(g_safe && (g_frame & 4));
    int32_t sx = g_px >> 4, sy = g_py >> 4;
    for (int16_t x = 0; x < W; x++) {
        uint32_t wx = g_scroll + (uint32_t)x;
        int16_t top = g_land[wx & (RING - 1)], prev = x ? g_land[(wx - 1) & (RING - 1)] : top;
        int16_t edge = prev < top ? prev : top, low = prev < top ? top : prev;   // a step: the trace's upright, up or down
        int16_t a = (int16_t)(edge - TOP), b = (int16_t)(low + 2 - TOP);
        bool gridX = x % 40 == 20;
        for (int16_t y = 0; y < a; y++)
            g_col[y] = (gridX && !(y & 3)) || ((y % 40 == 22) && !(x & 3)) ? P[C_GRID] : P[C_SKY];
        for (int16_t y = a < 0 ? 0 : a; y < b && y < H; y++) g_col[y] = P[C_TRACE];
        for (int16_t y = b; y < H; y++) g_col[y] = P[C_GROUND];

        for (const Target& t : g_tg) if (t.on) {
            const Sprite& s = t.kind == T_MAST ? MAST : CHIP;
            spriteCol(s, (int32_t)(t.wx - g_scroll), t.y - s.h, x);
        }
        for (const Thing& f : g_foe) if (f.on) spriteCol(f.kind == E_DART ? DART : GLITCH, f.x >> 4, f.y >> 4, x);
        for (const Thing& s : g_shot) if (s.on) boxCol(s.x >> 4, s.y >> 4, 6, 2, x, P[C_TEXT]);
        for (const Thing& s : g_bomb) if (s.on) boxCol(s.x >> 4, s.y >> 4, 3, 4, x, P[C_TEXT]);
        for (const Thing& s : g_hit) if (s.on) boxCol(s.x >> 4, s.y >> 4, 3, 3, x, P[C_ERR]);
        for (const Boom& e : g_boom) if (e.on) {
            int32_t cx = e.ground ? (int32_t)(e.x - (int32_t)g_scroll) : e.x;
            int32_t r = 2 + e.age * e.size / 18, dx = x - cx;
            if (dx < 0) dx = -dx;
            if (dx > r) continue;
            uint16_t c = (e.age & 2) ? P[C_ERR] : P[C_MARK];
            for (int32_t dy = -(r - dx); dy <= r - dx; dy++) {
                int32_t y = e.y + dy - TOP;
                if (y >= 0 && y < H && (dx + (dy < 0 ? -dy : dy) >= r - 3 || e.age < 6)) g_col[y] = c;
            }
        }
        if (showShip) spriteCol(SHIP, sx, sy, x);
        tft.pushColumn(x, TOP, H, g_col);
    }
}

static void title(Lcd& tft) {
    tft.fillScreen(TH.bg);
    // a strip of signal under the name
    for (int16_t x = 0; x < W; x++) {
        int16_t y = (int16_t)(196 - isin((uint8_t)(x * 3)) * 18 / 127);
        if ((x / 40) & 1) y = (x % 40) < 20 ? 182 : 210;
        tft.fillRect(x, y, 1, 2, TH.trace);
        tft.fillRect(x, y + 2, 1, 240 - y - 2, mix(TH.bg, TH.trace, 1));
    }
    Din::Text t(tft, Din::BIG, 0, 28);
    const char* name = "WAVE RAID";
    int16_t w = 0;
    for (const char* p = name; *p; p++) w += Din::advance(Din::BIG, *p);
    t.at((int16_t)((W - w) / 2), 28);
    t.color(TH.trace, TH.bg);
    t.print(name);
    Din::Text s(tft, Din::SMALL, 0, 66);
    const char* sub = "fly over the signal";
    w = 0;
    for (const char* p = sub; *p; p++) w += Din::advance(Din::SMALL, *p);
    s.at((int16_t)((W - w) / 2), 66);
    s.color(TH.text2, TH.bg);
    s.print(sub);
    tft.setTextSize(1);
    tft.setTextColor(TH.muted, TH.bg);
    tft.setCursor(52, 118); tft.print("arrows fly    RUN fire    OK bomb");
    tft.setCursor(34, 132); tft.print("AUTO autopilot (held: pause)   MODE menu");
    tft.setTextColor(TH.text, TH.bg);
    char b[24]; snprintf(b, sizeof(b), "HI %06lu", (unsigned long)g_hi);
    tft.setCursor(130, 150); tft.print(b);
    tft.setTextColor(TH.err, TH.bg);
    tft.setCursor(118, 166); tft.print("RUN to start");
    for (int16_t i = 0; i < SHIP_H; i++) for (int16_t j = 0; j < SHIP_W; j++) {   // the ship, twice its size
        char c = SHIP_ROWS[i][j];
        if (c != '.') tft.fillRect((int16_t)(140 + j * 2), (int16_t)(93 + i * 2), 2, 2, inkOf(c));   // between the line and the keys
    }
}

static void over(Lcd& tft) {
    tft.fillRect(70, 90, 180, 62, TH.bar);
    tft.drawRect(70, 90, 180, 62, TH.barHi);
    Din::Text t(tft, Din::SMALL, 0, 104);
    const char* s = "GAME OVER";
    int16_t w = 0;
    for (const char* p = s; *p; p++) w += Din::advance(Din::SMALL, *p);
    t.at((int16_t)(160 - w / 2), 104);
    t.color(TH.barHi, TH.bar);
    t.print(s);
    tft.setTextSize(1);
    tft.setTextColor(TH.barText, TH.bar);
    char b[24]; snprintf(b, sizeof(b), "SCORE %06lu", (unsigned long)g_score);
    tft.setCursor(124, 124); tft.print(b);
    tft.setCursor(86, 138); tft.print("RUN again    MODE menu");
}

void enter(Lcd& tft) {
    Acq::pause();                     // the converter's interrupt has nothing to do here
    colours();
    g_state = TITLE;
    title(tft);
    Keys::sync();
}

bool poll(Lcd& tft) {
    if (Keys::hit(Keys::MODE)) return false;
    uint32_t now = millis();
    switch (g_state) {
        case TITLE:
        case OVER:
            if (Keys::hit(Keys::RUN) || Keys::hit(Keys::OK)) {
                reset();
                g_state = PLAY;
                tft.fillRect(0, 0, W, TOP, TH.bar);
                bar(tft, true);
                g_lastMs = now;
                Keys::sync();
            }
            return true;
        case PAUSE:
            if (Keys::press(Keys::AUTO)) { g_state = PLAY; bar(tft, true); g_lastMs = now; }
            return true;
        default:
            break;
    }
    uint8_t a = Keys::press(Keys::AUTO);                      // short: the autopilot, held: pause
    if (a == Keys::LONG) { g_state = PAUSE; bar(tft, true); return true; }
    if (a == Keys::SHORT && g_state == PLAY) g_auto = !g_auto;
    if (now - g_lastMs < FRAME_MS) return true;
    g_lastMs = now - ((now - g_lastMs) > 2 * FRAME_MS ? 0 : (now - g_lastMs) - FRAME_MS);
    step();
    render(tft);
    bar(tft, false);
    if (g_state == OVER) over(tft);
    return true;
}

}  // namespace Game
