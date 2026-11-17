// Logger (Logger.h). Two channels: the multimeter (V, mA or A, by the knob) and
// the scope input as a DC voltmeter (ScopeDc.h). Their readings kept apart, in
// millionths of the unit (microvolts, nanoamps on mA), with their time. The
// graph is drawn column by column (no flicker) at each new reading, a reading
// holding until the next one (as a display does), each channel scaled to
// what it shows.
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include "Logger.h"
#include "Meter.h"
#include "ScopeDc.h"
#include "Theme.h"
#include "Keys.h"
#include "Link.h"
#include "Din.h"

namespace Logger {

static const uint32_t SPAN_MS = 60000;
static const uint16_t MAXN    = 420;          // 60 s at the ~5.5 frames a second the meter sends, and some
static const int32_t  LIMIT   = 800000000;    // readings kept within this, so the scale's sums stay in 32 bits

// the graphs: their left edge, the top of the first, the height of all, the
// widest; where the time labels go
static const int16_t GX = 44, GY = 96, GH = 112, GW_MAX = 270;
static const int16_t TIME_Y = GY + GH + 4;

enum { REC60, LIVE };
enum { READY, REC, DONE };
enum { L_SPLIT, L_OVER, L_METER, L_SCOPE, L_COUNT };   // the layouts, DOWN/UP
enum { M, S };                                          // the channels: meter, scope

static uint8_t  g_view = REC60;
static uint8_t  g_layout = L_SPLIT;
static uint8_t  g_state;
static bool     g_hold;               // live: the graph stands still
static uint8_t  g_pos;                // the knob position the meter's readings belong to
static uint8_t  g_barPos = 0xFF, g_barRange = 0xFF;   // what the bar names
static uint32_t g_t0;                 // the start of the recording
static uint32_t g_seq[2];             // the last reading taken, each channel
static uint32_t g_drawMs;
static bool     g_dirty;              // something new to draw
static uint8_t  g_digitW;             // the widest digit of the big font: every digit gets that cell
static uint8_t  g_hint = 0xFF;        // what the meter's value says, so it is drawn only when it changes
static uint32_t g_shownM, g_shownS;   // the readings on the screen

struct Chan {
    uint16_t n, head;                 // readings kept; where the next goes (a ring when live)
    uint32_t t[MAXN];                 // millis()
    int32_t  u[MAXN];                 // millionths of the unit
    int32_t  lo, hi;                  // the scale
    int32_t  labLo, labHi;            // the scale on the screen
    int16_t  ya[GW_MAX], yb[GW_MAX];  // each column's ink, top and bottom (rows from the graph's top)
};
static Chan g_ch[2];

static bool usable(uint8_t pos) { return pos == Meter::POS_V || pos == Meter::POS_MA || pos == Meter::POS_A; }
static const char* unitOf(uint8_t ch) {
    if (ch == S) return "V";
    return g_pos == Meter::POS_MA ? "mA" : g_pos == Meter::POS_A ? "A" : "V";
}
static uint16_t colorOf(uint8_t ch) { return ch == M ? TH.trace : TH.mark; }
static bool shows(uint8_t ch) { return g_layout == L_SPLIT || g_layout == L_OVER || g_layout == (ch == M ? L_METER : L_SCOPE); }
static int16_t gw() { return g_layout == L_OVER ? 226 : GW_MAX; }   // room on the right for the scope's scale

// a graph's rows: split, the meter on top and the scope under it
static void area(uint8_t ch, int16_t& y, int16_t& h) {
    if (g_layout == L_SPLIT) { h = (GH - 4) / 2; y = ch == M ? GY : (int16_t)(GY + h + 4); }
    else { y = GY; h = GH; }
}

// a meter reading in millionths of the knob's unit: uV on V, nA on mA, uA on A
static int32_t toMicro(const Meter::Value& v) {
    int k = 6 + v.exp - (g_pos == Meter::POS_MA ? -3 : 0) - v.dp;
    int64_t x = v.n;
    for (; k > 0; k--) x *= 10;
    for (; k < 0; k++) x = (x + (x < 0 ? -5 : 5)) / 10;
    if (x > LIMIT) x = LIMIT;
    if (x < -LIMIT) x = -LIMIT;
    return (int32_t)x;
}

// v in steps of 10^-dp, as text ("-0.0527")
static void fmtFixed(char* b, size_t size, int64_t v, uint8_t dp) {
    bool neg = v < 0;
    if (neg) v = -v;
    long div = 1;
    for (uint8_t i = 0; i < dp; i++) div *= 10;
    if (dp) snprintf(b, size, "%s%ld.%0*ld", neg ? "-" : "", (long)(v / div), dp, (long)(v % div));
    else    snprintf(b, size, "%s%ld", neg ? "-" : "", (long)v);
}

// millionths as the unit, with dp decimals (4 at most)
static void fmtMicro(char* b, size_t size, int32_t u, uint8_t dp) {
    if (dp > 4) dp = 4;
    static const int32_t P[5] = { 1000000, 100000, 10000, 1000, 100 };
    int32_t p = P[dp];
    fmtFixed(b, size, ((int64_t)u + (u < 0 ? -p / 2 : p / 2)) / p, dp);
}

static void printMicro(Print& out, int32_t u, uint8_t dp) {
    char b[16];
    fmtMicro(b, sizeof(b), u, dp);
    out.print(b);
}

static uint16_t at(const Chan& c, uint16_t k) { return (uint16_t)((c.head + MAXN - c.n + k) % MAXN); }

static void push(Chan& c, uint32_t t, int32_t u) {
    c.t[c.head] = t;
    c.u[c.head] = u;
    c.head = (uint16_t)((c.head + 1) % MAXN);
    if (c.n < MAXN) c.n++;
}

static void clearChan(Chan& c) {
    c.n = c.head = 0;
    c.lo = -5000; c.hi = 5000;
    c.labLo = c.labHi = 0;
}

// -- the frame -----------------------------------------------------------

static void bar(Lcd& tft) {
    tft.fillRect(0, 0, 320, 20, TH.bar);
    tft.setTextSize(1);
    tft.setTextColor(TH.barText, TH.bar);
    tft.setCursor(2, 6);
    tft.print("Logger");
    tft.setCursor(50, 6);
    if (g_view == LIVE) {
        if (g_hold) { tft.setTextColor(TH.err, TH.bar); tft.print("HOLD"); }
        else tft.print("live, 60 s");
    }
    else if (g_state == READY) tft.print("ready, 60 s");
    else if (g_state == DONE)  tft.print("done");
    else {
        tft.setTextColor(TH.err, TH.bar);
        tft.print("REC ");
        tft.print((millis() - g_t0) / 1000);
        tft.print(" s");
    }
    uint8_t pos = Meter::position(), r = ScopeDc::range();
    tft.setCursor(170, 6);
    tft.setTextColor(TH.trace, TH.bar);
    tft.print("meter ");
    tft.print(pos == Meter::POS_MA ? "mA" : pos == Meter::POS_A ? "A" : pos == Meter::POS_V ? "V" : "-");
    tft.setCursor(250, 6);
    tft.setTextColor(TH.mark, TH.bar);
    tft.print("scope R");
    tft.print(r);
    g_barPos = pos;
    g_barRange = r;
}

static void footer(Lcd& tft) {
    tft.fillRect(0, 226, 320, 14, TH.bg);
    tft.setTextSize(1);
    tft.setTextColor(TH.muted, TH.bg);
    tft.setCursor(4, 229);
    if (g_view == LIVE) tft.print(g_hold ? "RUN go on  AUTO record  DOWN view  MODE menu"
                                         : "RUN hold  AUTO record  DOWN view  MODE menu");
    else tft.print(g_state == REC ? "RUN again  AUTO live  DOWN view  MODE menu"
                                  : "RUN record 60 s  AUTO live  DOWN view  MODE menu");
}

// min, max, mean of what a channel shows, on one line of 52 characters at
// most: with wide figures the gaps shrink, then the count goes.
static void stats(Lcd& tft, uint8_t ch, int16_t y, uint16_t n, int32_t mn, int32_t mx, int32_t avg) {
    tft.fillRect(0, y - 2, 320, 12, TH.bg);
    tft.setTextSize(1);
    tft.setCursor(4, y);
    tft.setTextColor(colorOf(ch), TH.bg);
    tft.print(ch == M ? "meter" : "scope");
    if (!n) return;
    uint8_t dp = ch == M ? 4 : 3;
    char a[16], b[16], c[16], k[8];
    fmtMicro(a, sizeof(a), mn, dp);
    fmtMicro(b, sizeof(b), mx, dp);
    fmtMicro(c, sizeof(c), avg, dp);
    snprintf(k, sizeof(k), "%u", n);
    const char* u = unitOf(ch);
    // "meter min A  max B  avg C U  n K"
    int len = 5 + 5 + (int)strlen(a) + 6 + (int)strlen(b) + 6 + (int)strlen(c) + 1 + (int)strlen(u) + 4 + (int)strlen(k);
    bool tight = len > 52, count = len - 3 <= 52;
    const char* gap = tight ? " " : "  ";
    tft.setTextColor(TH.muted, TH.bg); tft.print(" min ");
    tft.setTextColor(TH.text2, TH.bg); tft.print(a);
    tft.setTextColor(TH.muted, TH.bg); tft.print(gap); tft.print("max ");
    tft.setTextColor(TH.text2, TH.bg); tft.print(b);
    tft.setTextColor(TH.muted, TH.bg); tft.print(gap); tft.print("avg ");
    tft.setTextColor(TH.text2, TH.bg); tft.print(c);
    tft.setTextColor(TH.muted, TH.bg); tft.print(' '); tft.print(u);
    if (count) { tft.print(gap); tft.print("n "); tft.print(k); }
}

static int16_t statsY(uint8_t ch) {
    if (g_layout == L_SPLIT || g_layout == L_OVER) return ch == M ? 68 : 80;
    return 74;
}

// The scale for what lies between mn and mx: a tenth more each way, at least
// 10 thousandths of the unit tall (10 mV, 10 uA).
static void scaleFor(Chan& c, int32_t mn, int32_t mx) {
    int32_t m = (mx - mn) / 10;
    c.lo = mn - m; c.hi = mx + m;
    if (c.hi - c.lo < 10000) {
        int32_t mid = mn / 2 + mx / 2;
        c.lo = mid - 5000; c.hi = mid + 5000;
    }
}

// A number's width in the built-in font (6 px a character).
class Count : public Print {
public:
    size_t write(uint8_t) override { n++; return 1; }
    uint8_t n = 0;
};

// as many decimals as fit in 6 characters
static void scaleLabel(Lcd& tft, int32_t u, int16_t x, int16_t y, bool right) {
    uint8_t dp = 3;
    Count c;
    for (;; dp--) {
        c.n = 0;
        printMicro(c, u, dp);
        if (c.n <= 6 || dp == 0) break;
    }
    tft.setCursor(right ? x - 6 * c.n : x, y);
    printMicro(tft, u, dp);
}

// a channel's scale beside its graph: the meter's on the left, the scope's on
// the right when they share one
static void scaleLabels(Lcd& tft, uint8_t ch) {
    Chan& c = g_ch[ch];
    int16_t y, h;
    area(ch, y, h);
    bool right = g_layout == L_OVER && ch == S;
    int16_t x0 = right ? (int16_t)(GX + gw() + 1) : 0, w = right ? (int16_t)(320 - x0) : (int16_t)(GX - 2);
    tft.fillRect(x0, y - 1, w, 9, TH.bg);
    tft.fillRect(x0, y + h - 9, w, 9, TH.bg);
    c.labLo = c.lo; c.labHi = c.hi;
    if (!c.n) return;
    tft.setTextSize(1);
    tft.setTextColor(g_layout == L_OVER ? colorOf(ch) : TH.muted, TH.bg);
    int16_t x = right ? (int16_t)(GX + gw() + 3) : (int16_t)(GX - 3);
    scaleLabel(tft, c.hi, x, y, !right);
    scaleLabel(tft, c.lo, x, (int16_t)(y + h - 8), !right);
}

static void frames(Lcd& tft) {
    tft.fillRect(0, GY - 6, 320, GH + 20, TH.bg);
    for (uint8_t ch = 0; ch < 2; ch++) {
        if (!shows(ch) || (g_layout == L_OVER && ch == S)) continue;
        int16_t y, h;
        area(ch, y, h);
        tft.drawRect(GX - 1, y - 1, gw() + 2, h + 2, TH.line);
    }
    tft.setTextSize(1);
    tft.setTextColor(TH.muted, TH.bg);
    for (uint8_t s = 0; s <= 60; s += 10) {
        int16_t x = (int16_t)(GX + (int32_t)s * (gw() - 1) / 60);
        char t[5];
        snprintf(t, sizeof(t), "%d", g_view == LIVE ? s - 60 : s);   // live: seconds ago
        int16_t w = (int16_t)(6 * strlen(t)), cx = x - w / 2;
        if (cx + w > 319) cx = 319 - w;
        tft.setCursor(cx, TIME_Y);
        tft.print(t);
    }
    for (uint8_t ch = 0; ch < 2; ch++) if (shows(ch)) scaleLabels(tft, ch);
}

// -- the graphs ----------------------------------------------------------

static void mark(Chan& c, int16_t col, int16_t a, int16_t b) {
    if (col < 0 || col >= gw()) return;
    if (a > b) { int16_t t = a; a = b; b = t; }
    if (a < c.ya[col]) c.ya[col] = a;
    if (b > c.yb[col]) c.yb[col] = b;
}

// A channel's columns, scale and figures, for the time from tEnd - 60 s to
// tEnd; the last reading holds up to `upTo`.
static void inkOf(Lcd& tft, uint8_t ch, uint32_t tEnd, uint32_t upTo) {
    Chan& c = g_ch[ch];
    int16_t W = gw(), y0, h;
    area(ch, y0, h);
    int32_t mn = INT32_MAX, mx = INT32_MIN;
    int64_t sum = 0;
    uint16_t cnt = 0, first = c.n;
    for (uint16_t k = 0; k < c.n; k++) {
        uint16_t i = at(c, k);
        if ((int32_t)(c.t[i] - (tEnd - SPAN_MS)) < 0) continue;
        if (first == c.n) first = k;
        cnt++;
        sum += c.u[i];
        if (c.u[i] < mn) mn = c.u[i];
        if (c.u[i] > mx) mx = c.u[i];
    }
    stats(tft, ch, statsY(ch), cnt, mn, mx, cnt ? (int32_t)(sum / cnt) : 0);
    if (cnt) scaleFor(c, mn, mx);
    if (c.lo != c.labLo || c.hi != c.labHi) scaleLabels(tft, ch);

    for (int16_t col = 0; col < W; col++) { c.ya[col] = INT16_MAX; c.yb[col] = -1; }
    auto colOf = [&](uint32_t t) -> int16_t {
        int32_t back = (int32_t)(tEnd - t);
        if (back < 0) back = 0;
        return (int16_t)(W - 1 - (int32_t)((uint64_t)back * (W - 1) / SPAN_MS));
    };
    auto rowOf = [&](int32_t u) -> int16_t {
        int64_t d = ((int64_t)u - c.lo) * (h - 1) / ((int64_t)c.hi - c.lo);
        if (d < 0) d = 0;
        if (d > h - 1) d = h - 1;
        return (int16_t)(h - 1 - d);
    };
    int16_t px = -1, py = 0;
    for (uint16_t k = first; k < c.n; k++) {
        uint16_t i = at(c, k);
        int16_t x = colOf(c.t[i]), y = rowOf(c.u[i]);
        if (px >= 0) for (int16_t col = (int16_t)(px + 1); col < x; col++) mark(c, col, py, py);
        mark(c, x, px >= 0 ? py : y, y);
        px = x; py = y;
    }
    if (px >= 0) for (int16_t col = (int16_t)(px + 1), e = colOf(upTo); col <= e; col++) mark(c, col, py, py);
}

// One graph's area painted, column by column, with the channels it holds.
static void paint(Lcd& tft, int16_t y0, int16_t h, bool a, bool b, bool grid) {
    int16_t W = gw();
    for (int16_t col = 0; col < W; col++) {
        int16_t x = (int16_t)(GX + col);
        tft.fillRect(x, y0, 1, h, TH.bg);
        if (grid && col && col < W - 1 && (col * 6) % (W - 1) < 6)   // every 10 s
            for (int16_t y = 0; y < h; y += 4) tft.drawPixel(x, (int16_t)(y0 + y), TH.line);
        for (uint8_t ch = 0; ch < 2; ch++) {
            if (!(ch == M ? a : b)) continue;
            Chan& c = g_ch[ch];
            if (c.yb[col] < 0) continue;
            tft.fillRect(x, (int16_t)(y0 + c.ya[col]), 1, (int16_t)(c.yb[col] - c.ya[col] + 1), colorOf(ch));
        }
    }
}

static void draw(Lcd& tft, uint32_t now) {
    uint32_t tEnd = g_view == LIVE ? now : g_t0 + SPAN_MS, upTo = now;
    if (g_view == REC60) {
        if (g_state == READY) upTo = tEnd - SPAN_MS;     // nothing yet
        if ((int32_t)(upTo - tEnd) > 0) upTo = tEnd;
    }
    bool grid = g_view == REC60;
    for (uint8_t ch = 0; ch < 2; ch++) if (shows(ch)) inkOf(tft, ch, tEnd, upTo);
    int16_t y, h;
    if (g_layout == L_SPLIT) {
        area(M, y, h); paint(tft, y, h, true, false, grid);
        area(S, y, h); paint(tft, y, h, false, true, grid);
    } else {
        area(M, y, h);
        paint(tft, y, h, shows(M), shows(S), grid);
    }
}

// -- the big values ------------------------------------------------------

static uint8_t cellW(char c) {
    return (c >= '0' && c <= '9') ? g_digitW : Din::advance(Din::BIG, c);
}

// Set right, against `right`, every digit in a cell of one width: the figures
// stay where they are as the value changes, a minus comes in on the left.
static void bigText(Lcd& tft, const char* s, const char* unit, int16_t right, uint16_t col, uint16_t unitCol) {
    int16_t w = 0;
    for (const char* p = s; *p; p++) w += cellW(*p);
    int16_t x = (int16_t)(right - w);
    Din::Text t(tft, Din::BIG, x, 34);
    t.color(col, TH.bg);
    for (const char* p = s; *p; p++) {
        uint8_t c = cellW(*p);
        t.at((int16_t)(x + (c - Din::advance(Din::BIG, *p)) / 2), 34);
        t.write((uint8_t)*p);
        x += c;
    }
    t.at((int16_t)(right + 5), 34);
    t.color(unitCol, TH.bg);
    t.print(unit);
}

// the value cells: one over the whole width, or the meter on the left and
// the scope on the right
static bool both() { return g_layout == L_SPLIT || g_layout == L_OVER; }

// On V the meter's value in volts with 4 decimals; on mA and A what the meter
// shows, its digits and its unit (it ranges by itself: uA, mA, A).
static void bigMeter(Lcd& tft, bool force) {
    if (!shows(M)) return;
    const Meter::Value& v = Meter::value();
    uint8_t pos = Meter::position();
    uint8_t hint = !v.seq ? 2 : !usable(pos) ? 1 : 0;
    if (!force && hint == g_hint && (hint != 0 || v.seq == g_shownM)) return;
    g_hint = hint;
    g_shownM = v.seq;
    int16_t x0 = 0, w = both() ? 160 : 320;
    tft.fillRect(x0, 22, w, 42, TH.bg);
    if (hint) {
        Din::Text t(tft, Din::SMALL, 6, 40);
        t.color(hint == 1 ? TH.err : TH.muted, TH.bg);
        if (both()) t.print(hint == 1 ? "knob: V, mA, A" : "meter starting");
        else t.print(hint == 1 ? "turn the knob to V, mA or A" : "multimeter starting...");
        return;
    }
    char b[16];
    const char* unit = pos == Meter::POS_V ? "V" : (v.unit[0] ? v.unit : pos == Meter::POS_A ? "A" : "mA");
    if (v.ol)                     strcpy(b, "OL");
    else if (!v.num)              strcpy(b, "---");
    else if (pos == Meter::POS_V) fmtMicro(b, sizeof(b), toMicro(v), 4);
    else                          fmtFixed(b, sizeof(b), v.n, v.dp);
    // in its half the unit ends short of the scope's value, which clears from x 160
    int16_t right = 236;
    if (both()) {
        int16_t uw = 0;
        for (const char* p = unit; *p; p++) uw += Din::advance(Din::BIG, *p);
        right = (int16_t)(155 - 5 - uw);
    }
    bigText(tft, b, unit, right, TH.trace, TH.trace);         // the colour of its trace
}

static void bigScope(Lcd& tft, bool force) {
    if (!shows(S)) return;
    const ScopeDc::Value& v = ScopeDc::value();
    if (!force && v.seq == g_shownS) return;
    g_shownS = v.seq;
    int16_t x0 = both() ? 160 : 0, w = both() ? 160 : 320;
    tft.fillRect(x0, 22, w, 42, TH.bg);
    if (!v.seq) return;
    char b[16];
    if (v.clipped) strcpy(b, "OL");                           // past the converter: +-50 V on R9
    else fmtMicro(b, sizeof(b), v.uv, 3);
    bigText(tft, b, "V", both() ? 290 : 236, TH.mark, TH.mark);
}

// -- recording and live --------------------------------------------------

static void startRec(Lcd& tft) {
    g_state = REC;
    g_t0 = millis();
    g_pos = Meter::position();
    clearChan(g_ch[M]); clearChan(g_ch[S]);
    char b[100];
    snprintf(b, sizeof(b), "{\"t\":\"log\",\"start\":true,\"spanMs\":60000,\"meter\":\"%s\",\"scope\":\"V\"}",
             usable(g_pos) ? unitOf(M) : "");
    Link::send(b);
    bar(tft);
    frames(tft);
    footer(tft);
    g_dirty = true;
}

static void endFigures(char* b, size_t size, const char* name, uint8_t ch) {
    Chan& c = g_ch[ch];
    int32_t mn = 0, mx = 0;
    int64_t sum = 0;
    for (uint16_t k = 0; k < c.n; k++) {
        int32_t u = c.u[at(c, k)];
        if (!k || u < mn) mn = u;
        if (!k || u > mx) mx = u;
        sum += u;
    }
    snprintf(b, size, "\"%s\":{\"n\":%u,\"min\":%ld,\"max\":%ld,\"avg\":%ld}", name, c.n,
             (long)mn, (long)mx, (long)(c.n ? sum / c.n : 0));
}

static void finish(Lcd& tft) {
    g_state = DONE;
    char m[90], s[90], b[200];
    endFigures(m, sizeof(m), "meter", M);
    endFigures(s, sizeof(s), "scope", S);
    snprintf(b, sizeof(b), "{\"t\":\"log\",\"end\":true,%s,%s}", m, s);
    Link::send(b);
    bar(tft);
    footer(tft);
    g_dirty = true;
}

// the new readings of both channels, kept (and, recording, sent)
static void take(uint32_t now) {
    bool rec = g_view == REC60;
    if (rec && g_state != REC) {                       // ready or done: looked at, not kept
        g_seq[M] = Meter::value().seq;
        g_seq[S] = ScopeDc::value().seq;
        return;
    }
    if (rec && now - g_t0 >= SPAN_MS) return;
    const Meter::Value& v = Meter::value();
    if (v.seq != g_seq[M]) {
        g_seq[M] = v.seq;
        if (Meter::position() == g_pos && usable(g_pos) && v.num && !v.ol && (!rec || g_ch[M].n < MAXN)) {
            int32_t u = toMicro(v);
            push(g_ch[M], now, u);
            g_dirty = true;
            if (rec) {
                char b[80];
                snprintf(b, sizeof(b), "{\"t\":\"log\",\"ch\":\"meter\",\"ms\":%lu,\"u\":%ld}", (unsigned long)(now - g_t0), (long)u);
                Link::send(b);
            }
        }
    }
    const ScopeDc::Value& s = ScopeDc::value();
    if (s.seq != g_seq[S]) {
        g_seq[S] = s.seq;
        if (!rec || g_ch[S].n < MAXN) {
            push(g_ch[S], now, s.uv);
            g_dirty = true;
            if (rec) {
                char b[80];
                snprintf(b, sizeof(b), "{\"t\":\"log\",\"ch\":\"scope\",\"ms\":%lu,\"u\":%ld}", (unsigned long)(now - g_t0), (long)s.uv);
                Link::send(b);
            }
        }
    }
}

// -- the screen ----------------------------------------------------------

static void redraw(Lcd& tft) {
    tft.fillScreen(TH.bg);
    bar(tft);
    frames(tft);
    footer(tft);
    g_hint = 0xFF;
    bigMeter(tft, true);
    bigScope(tft, true);
    g_dirty = true;
}

static void show(Lcd& tft) {
    g_state = READY;
    g_hold = false;
    g_pos = Meter::position();
    clearChan(g_ch[M]); clearChan(g_ch[S]);
    g_seq[M] = Meter::value().seq;
    g_seq[S] = ScopeDc::value().seq;
    redraw(tft);
}

void enter(Lcd& tft) {
    Meter::begin();
    ScopeDc::begin();
    g_digitW = 0;
    for (char c = '0'; c <= '9'; c++) {
        uint8_t a = Din::advance(Din::BIG, c);
        if (a > g_digitW) g_digitW = a;
    }
    show(tft);
    Keys::sync();
}

void leave() {
    Meter::end();
    ScopeDc::end();
}

bool poll(Lcd& tft) {
    if (Keys::hit(Keys::MODE) || Keys::press(Keys::OK) == Keys::LONG) return false;
    ScopeDc::poll();
    uint32_t now = millis();

    bigMeter(tft, false);
    bigScope(tft, false);
    if (Meter::position() != g_barPos || ScopeDc::range() != g_barRange) bar(tft);

    if (Keys::hit(Keys::AUTO)) {
        if (g_view == REC60 && g_state == REC) finish(tft);
        g_view = g_view == LIVE ? REC60 : LIVE;
        show(tft);
        return true;
    }
    uint8_t step = Keys::hit(Keys::DOWN) ? 1 : Keys::hit(Keys::UP) ? L_COUNT - 1 : 0;
    if (step) {
        g_layout = (uint8_t)((g_layout + step) % L_COUNT);
        redraw(tft);
        return true;
    }
    if (Keys::hit(Keys::RUN)) {
        if (g_view == LIVE) { g_hold = !g_hold; bar(tft); footer(tft); }
        else startRec(tft);
    }

    if (g_view == LIVE && Meter::position() != g_pos) {   // another unit: the meter's graph starts again
        g_pos = Meter::position();
        clearChan(g_ch[M]);
        g_dirty = true;
    }
    take(now);

    if (g_view == REC60 && g_state == REC) {
        static uint32_t sec;
        if ((now - g_t0) / 1000 != sec) { sec = (now - g_t0) / 1000; bar(tft); }
        if (now - g_t0 >= SPAN_MS) finish(tft);
    }
    bool moving = (g_view == LIVE && !g_hold) || (g_view == REC60 && g_state == REC);
    if ((g_dirty && !(g_view == LIVE && g_hold)) || (moving && now - g_drawMs >= 250)) {
        g_dirty = false;
        g_drawMs = now;
        draw(tft, now);
    }
    return true;
}

}  // namespace Logger
