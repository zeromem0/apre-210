// What the start screen says this firmware is, so that one build is not
// taken for another once several are about. Raise FW_VERSION with each build
// that goes to an instrument; the date is the compiler's.

#ifndef VERSION_H
#define VERSION_H

#define FW_NAME     "APRE-210"
#if defined(ZX)
#define FW_VERSION "0.27zx"     // flash_zx: flash_apre with the Spectrum (src/zx), local
#elif defined(NO_USB_SENDER)
#define FW_VERSION "0.27"       // flash_apre: from apre-510 v0.30 on the DST-210 board
#else
#define FW_VERSION "0.27u"      // flash_t232, ram_t232: with the USB Sender, local
#endif
#define FW_DATE     __DATE__

#endif
