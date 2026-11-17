// The multimeter probe (Meter.h). Everything here is read out of the FNIRSI
// firmware V0.1.0 and not yet seen working: the USART1 setup at 0x0801F0B0,
// the interrupt at 0x080218B0, the sending task at 0x08031C70, the decoding
// task at 0x08030D88, the rotary switch in the keyboard scan at 0x08032FA8.
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include "Meter.h"
#include "Pins.h"
#include "Link.h"

namespace Meter {

static const uint8_t FUNC[10] = { 0x14, 0x0C, 0x0B, 0x17, 0x0E, 0x0A, 0x12, 0x0F, 0x11, 0x10 };  // 0x08038C3A

static bool     g_on;
static uint8_t  g_pos;                  // the rotary switch, 1..10
static uint32_t g_posMs;
static uint8_t  g_tx[10];
static uint8_t  g_txN = 10;             // sent so far
static uint32_t g_frames, g_bad;
static Value    g_val;
static bool     g_ready;                // the chip has answered since it was powered (it takes ~1.5 s)
static uint16_t g_pending;              // a command waiting for the chip to be ready
static uint16_t g_sent;                 // the last command sent, until the chip acknowledges it
static uint32_t g_sentMs;
static uint8_t  g_tries;

// frames off the line, filled in the interrupt: 12 bytes from 5A A5 (the
// readings), 10 from AA 55 (the chip answering a command: FNIRSI's interrupt
// drops those, so their meaning is a guess)
static volatile uint8_t g_rx[12];
static volatile uint8_t g_rxN;
static uint8_t  g_ring[8][12];
static volatile uint8_t g_head, g_tail;

static uint32_t pclk2() {
    uint32_t pre = (RCC->CFGR >> 11) & 7;           // APB2 prescaler, as on the STM32F1
    return pre >= 4 ? SystemCoreClock >> (pre - 3) : SystemCoreClock;
}

}  // namespace Meter

extern "C" void USART1_IRQHandler(void) {
    using namespace Meter;
    uint32_t sr = USART1->SR;
    if (sr & (USART_SR_RXNE | USART_SR_ORE)) {
        uint8_t c = (uint8_t)USART1->DR;               // also clears an overrun
        uint8_t n = g_rxN;
        if (n == 0) { if (c == 0x5A || c == 0xAA) { g_rx[0] = c; g_rxN = 1; } return; }
        if (n == 1) {
            bool ok = (g_rx[0] == 0x5A && c == 0xA5) || (g_rx[0] == 0xAA && c == 0x55);
            if (!ok) { if (c == 0x5A || c == 0xAA) g_rx[0] = c; else g_rxN = 0; return; }
        }
        g_rx[n++] = c;
        if (n == (g_rx[0] == 0x5A ? 12 : 10)) {
            uint8_t h = g_head, next = (uint8_t)((h + 1) & 7);
            if (next != g_tail) {
                for (uint8_t i = 0; i < n; i++) g_ring[h][i] = g_rx[i];
                g_head = next;
            }
            n = 0;
        }
        g_rxN = n;
    }
}

namespace Meter {

// The rotary switch: PA15 low is position 1; otherwise PA1, PB13, PD3 pulled
// low give which of PC13..PC15 (4/2/1) is closed, then PC13..PC15 pulled low
// give which of PA1, PD3, PB13 (FNIRSI 0x08032FA8).
static uint8_t readSwitch() {
    const Pins::Pin rows[3] = { PIN_A(1), PIN_D(3), PIN_B(13) };
    const Pins::Pin cols[3] = { PIN_C(13), PIN_C(14), PIN_C(15) };
    Pins::inputPullUp(PIN_A(15));
    for (const Pins::Pin& c : cols) Pins::inputPullUp(c);
    for (const Pins::Pin& r : rows) Pins::output(r, false);
    delayMicroseconds(20);
    bool p1 = !Pins::read(PIN_A(15));
    uint8_t code = (!Pins::read(cols[0]) ? 1 : 0) | (!Pins::read(cols[1]) ? 2 : 0) | (!Pins::read(cols[2]) ? 4 : 0);
    for (const Pins::Pin& r : rows) Pins::inputPullUp(r);
    for (const Pins::Pin& c : cols) Pins::output(c, false);
    delayMicroseconds(20);
    bool a1 = !Pins::read(rows[0]), d3 = !Pins::read(rows[1]), b13 = !Pins::read(rows[2]);
    for (const Pins::Pin& c : cols) Pins::inputPullUp(c);
    for (const Pins::Pin& r : rows) Pins::inputPullUp(r);
    if (p1) return 1;
    static const uint8_t A1[5]  = { 0, 7, 8, 0, 4 };   // by code 1, 2, 4
    static const uint8_t D3[5]  = { 0, 9, 6, 0, 3 };
    static const uint8_t B13[5] = { 0, 10, 5, 0, 2 };
    if (code != 1 && code != 2 && code != 4) return 0;
    if (a1)  return A1[code];
    if (d3)  return D3[code];
    if (b13) return B13[code];
    return 0;
}

static void sendNow(uint16_t word) {
    g_tx[0] = 0xAA; g_tx[1] = 0x55;
    g_tx[2] = (uint8_t)(word >> 8); g_tx[3] = (uint8_t)word;
    for (uint8_t i = 4; i < 9; i++) g_tx[i] = 0;
    g_tx[9] = (uint8_t)(g_tx[2] + g_tx[3]);
    g_txN = 0;
    g_sent = word;
    g_sentMs = millis();
}

// Sent once the chip is up, and again (up to three times) if it does not
// answer AA 55: the first command, sent at power on, was lost. Only the
// function codes FNIRSI sends for the rotary switch go out: the keys' commands
// are not known, and 02A0 (ENTER held in FNIRSI) puts the chip in CAL, whose
// calibration lives in OTP (apre-atlas.md).
void command(uint16_t word) {
    bool known = false;
    for (uint8_t i = 0; i < 10; i++)
        if (word == (uint16_t)(0x0500 | FUNC[i])) known = true;
    if (!known) return;
    g_tries = 0;
    if (g_ready) sendNow(word);
    else g_pending = word;
}

void begin() {
    if (g_on) return;
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN | RCC_APB2ENR_IOPCEN | RCC_APB2ENR_IOPDEN
                  | RCC_APB2ENR_AFIOEN | RCC_APB2ENR_USART1EN;
    Pins::output(PIN_B(12), true);                    // the isolated side's power
    Pins::mode(PIN_A(9), 0xB);                        // TX: alternate function, push-pull
    Pins::inputPullUp(PIN_A(10));                     // RX
    USART1->CR1 = 0;
    USART1->BRR = (uint16_t)((pclk2() + 4800) / 9600);
    USART1->CR2 = 0;
    USART1->CR3 = 0;
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_UE;
    g_rxN = 0; g_head = g_tail = 0;
    NVIC_SetPriority(USART1_IRQn, 6);
    NVIC_EnableIRQ(USART1_IRQn);
    g_on = true;
    g_pos = 0;
    g_posMs = 0;
    g_frames = g_bad = 0;
    g_ready = false;
    g_pending = g_sent = 0;
    memset(&g_val, 0, sizeof(g_val));
}

void end() {
    if (!g_on) return;
    NVIC_DisableIRQ(USART1_IRQn);
    USART1->CR1 = 0;
    Pins::write(PIN_B(12), false);
    g_on = false;
}

bool on() { return g_on; }
const Value& value() { return g_val; }
uint8_t position() { return g_pos; }

// 7-segment byte (bit 4, the point, masked) to a character (FNIRSI 0x0802EC38)
static char segChar(uint8_t s) {
    switch (s & 0xEF) {
        case 0xEB: return '0'; case 0x0A: return '1'; case 0xAD: return '2'; case 0x8F: return '3';
        case 0x4E: return '4'; case 0xC7: return '5'; case 0xE7: return '6'; case 0x8A: return '7';
        case 0xEF: return '8'; case 0xCF: return '9'; case 0x00: return ' '; case 0x04: return '-';
        case 0x61: return 'L'; case 0xE1: return 'C'; case 0xE5: return 'E'; case 0xEE: return 'A';
        case 0x23: return 'u'; case 0x65: return 't'; case 0x27: return 'o'; case 0x24: return 'r';
        case 0xEC: return 'P';
        default:   return '?';
    }
}

static void report(const uint8_t* f) {
    char hex[25];
    if (f[0] == 0xAA) {                                // the chip's answer to a command
        for (uint8_t i = 0; i < 10; i++) sprintf(hex + 2 * i, "%02x", f[i]);
        // acknowledged: byte 3 repeats the code, byte 7 is AA (the chip answers
        // FD or FF in byte 2, not the command's first byte; OpenScope-2C53T has
        // the same rule)
        if (g_sent && f[3] == (uint8_t)g_sent && f[7] == 0xAA) g_sent = 0;
        char b[80];
        snprintf(b, sizeof(b), "{\"t\":\"meter\",\"pos\":%u,\"ack\":\"%s\"}", g_pos, hex);
        Link::send(b);
        return;
    }
    for (uint8_t i = 0; i < 12; i++) sprintf(hex + 2 * i, "%02x", f[i]);
    char dig[5];
    long n = 0;
    bool num = true;
    for (uint8_t i = 0; i < 4; i++) {
        uint8_t s = (uint8_t)((f[2 + i] & 0xF0) | (f[3 + i] & 0x0F));
        dig[i] = segChar(s);
        if (dig[i] >= '0' && dig[i] <= '9') n = n * 10 + (dig[i] - '0');
        else if (dig[i] == ' ') n = n * 10;
        else num = false;
    }
    dig[4] = 0;
    if (f[2] & 0x08) n += 10000;                     // the leading 1
    uint8_t dp = (f[3] & 0x10) ? 3 : (f[4] & 0x10) ? 2 : (f[5] & 0x10) ? 1 : 0;
    if (f[8] & 0x80) dp = 4;                         // byte 8 (0x200046E1 in FNIRSI), not 10
    bool neg = f[2] & 0x10;
    g_val.seq++;
    g_val.ms  = millis();
    g_val.num = num;
    g_val.ol  = dig[1] == '0' && dig[2] == 'L';
    g_val.n   = neg ? -n : n;
    g_val.dp  = dp;
    // the unit, as FNIRSI picks its label (0x08031190, the table at
    // 0x080390C8): on V, mV if b7 bit 0; on mA and A, uA if b6 bit 4, else mA
    // if b7 bit 0, else A
    const char* u = "";
    int8_t e = 0;
    if (g_pos == POS_V) { if (f[7] & 1) { u = "mV"; e = -3; } else u = "V"; }
    else if (g_pos == POS_MA || g_pos == POS_A) {
        if (f[6] & 0x10)     { u = "uA"; e = -6; }
        else if (f[7] & 1)   { u = "mA"; e = -3; }
        else                   u = "A";
    }
    g_val.exp = e;
    strncpy(g_val.unit, u, sizeof(g_val.unit));
    char b[200];
    snprintf(b, sizeof(b), "{\"t\":\"meter\",\"pos\":%u,\"d\":\"%s\",\"lcd\":\"%s%s\",\"n\":%ld,\"dp\":%u,\"num\":%s,\"unit\":\"%s\"}",
             g_pos, hex, neg ? "-" : "", dig, neg ? -n : n, dp, num ? "true" : "false", u);
    Link::send(b);
}

void poll() {
    if (!g_on) return;
    uint32_t now = millis();
    if (now - g_posMs >= 100) {
        g_posMs = now;
        uint8_t p = readSwitch();
        if (p && p != g_pos) {
            g_pos = p;
            command((uint16_t)(0x0500 | FUNC[p - 1]));
            char b[64];
            snprintf(b, sizeof(b), "{\"t\":\"meter\",\"pos\":%u,\"cmd\":\"05%02x\"}", p, FUNC[p - 1]);
            Link::send(b);
        }
    }
    while (g_txN < 10 && (USART1->SR & USART_SR_TXE)) USART1->DR = g_tx[g_txN++];
    while (g_tail != g_head) {
        g_frames++;
        if (g_ring[g_tail][0] == 0x5A) g_ready = true;
        report(g_ring[g_tail]);
        g_tail = (uint8_t)((g_tail + 1) & 7);
    }
    if (g_ready && g_pending) { sendNow(g_pending); g_pending = 0; }
    if (g_sent && g_txN >= 10 && now - g_sentMs > 600) {
        if (++g_tries < 3) sendNow(g_sent);
        else {
            char b[64];
            snprintf(b, sizeof(b), "{\"t\":\"meter\",\"noack\":\"%04x\"}", g_sent);
            Link::send(b);
            g_sent = 0;
        }
    }
}

}  // namespace Meter
