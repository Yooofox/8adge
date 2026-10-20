#ifndef EPD_398_4COLOR_H
#define EPD_398_4COLOR_H

#include <Arduino.h>

// ============================================================================
//  3.98" 4-color e-paper driver - A1 panel (black / white / red / yellow)
//
//  Panel:      768 x 552 pixels, 2 bits per pixel (4 colors)
//  Controller: JD79668 family, driven with the JD79665 register set
//  Bus:        4-wire bit-banged SPI (MSB first, clock idles LOW)
//  Revision:   A1 production revision only
//
//  Single-path final version. The reasoning below is kept because two of the
//  obvious "improvements" here make the picture worse, and both are easy to
//  reinvent from the open-source driver this register sequence came from.
//
//  ---------------------------------------------------------------- addressing
//
//  The controller's gate address space is 600 lines, but the panel has only 552
//  pixel rows, and its gate lines are wired in two interleaved groups:
//
//      image rows   0..299  ->  gate addresses 0, 2, 4, ..., 598   (300 rows)
//      image rows 300..551  ->  gate addresses 1, 3, 5, ..., 503   (252 rows)
//
//  So image row r sits at gate 2*r for r <= 299, and at 2*r - 599 above that,
//  and 0x61 has to be told VRES = 600 rather than 552.
//
//  With VRES = 552 the controller only drives gates 0..551, so image rows
//  276..299 (gate addresses 552..598) are never driven at all: the picture comes
//  out cut in half at the middle, the lower half pushed down, and the gap keeps
//  showing the previous frame. That is the failure the open-source driver has on
//  this panel, and it is why its own VRES = 552 cannot be copied here.
//
//  The 48 remaining gates (odd 505..599) have no pixel row. The controller
//  drives the two interleaved groups in pairs, so gates 504..598 each end up
//  paired with a dangling partner and the last 96 gates of the scan are
//  half-loaded. That is what used to leave image rows 252..299 (45.7%-54.3% down
//  the screen) visibly darker than the rest - most obvious on large flat areas
//  of a single colour, where the whole band shifts to a deeper tone.
//
//  ---------------------------------------------------------------- sequence
//
//  The register sequence is the JD79665 one from the open-source Huawei A1
//  adaptation (github.com/krstc/InkSight_adapt_HUAWEI_eink, MIT). It is what
//  removes that band: the vendor firmware's own sequence sets 0x00 <- 0x0B,
//  "LUT from OTP", which leaves the panel on an internal waveform that reacts to
//  the half-loaded gate pairs.
//
//  The register set and the addressing are coupled, so neither can be swapped on
//  its own: the vendor registers go with the interleaved addressing, and the
//  open-source registers were written for linear addressing. Measured on this
//  panel, every alternative to this file is wrong:
//
//      vendor registers      + VRES 600 + 552 single-row windows
//          -> correct image, but the darker band at 45.7%-54.3% is present
//      open-source registers + VRES 600 + 552 single-row windows
//          -> image stretched about 2x vertically, lower part off the panel
//      open-source registers + VRES 552 + linear burst
//          -> 24-row dead band at 50.0%-54.3% (image rows 276..299)
//      open-source registers + VRES 600 + single burst           <- THIS FILE
//          -> correct image, no darker band
//
//  A single 0x10 burst through the full window still walks the gate space in the
//  interleaved order, so the framebuffer order (image rows 0..551) lands on the
//  right gates without the driver having to issue 552 single-row 0x83 windows.
//
//  ---------------------------------------------------------------- refresh
//
//  BUSY can go HIGH before the waveform has finished, so a release in under
//  EPD398_BUSY_EARLY_MS is treated as "not finished" and followed by a fixed
//  EPD398_REFRESH_FALLBACK_MS wait before power-off.
//
//  That fallback wait must exceed the longest real refresh (15-25 s). A fixed
//  wait that is shorter powers the panel off while the last rows are still being
//  written, and those rows come out over-driven. The open-source driver uses
//  14000 ms here, which sits inside that range and is therefore unsafe; 25000 ms
//  is used instead. 0x12 takes 0x00, as in the open-source driver.
//
//  ---------------------------------------------------------------- commands
//
//      0x00 panel setting        0x04 power ON          0x12 display refresh
//      0x02 power OFF            0x07 deep sleep        0x61 resolution
//      0x10 data start xmit      0x83 partial window
//
//  This file is self-contained: if the host project already defines the
//  PIN_EPD_* macros (e.g. 8adge's config.h) those are used, otherwise the
//  defaults below (the vendor's wiring) apply.
// ============================================================================

// ---------------------------------------------------------------- geometry
#define EPD398_WIDTH        768
#define EPD398_HEIGHT       552
// 2 bits per pixel, the panel's native packing: 4 pixels per byte, most
// significant pair leftmost. Reported to the app in GET_INFO.
#define EPD398_BPP          2
#define EPD398_ROW_BYTES    (EPD398_WIDTH / 4)                  // 192 bytes/row @ 2bpp
#define EPD398_COLOR_BYTES  (EPD398_WIDTH * EPD398_HEIGHT / 4)  // 105984 bytes

// Controller gate address space. 600, NOT EPD398_HEIGHT - see the addressing
// note above. A 552-line space cannot reach the gate addresses of image rows
// 276..299, which is a visible defect rather than a rounding difference.
#define EPD398_VRES         600

// Both bounds are compile-time, because the failure they prevent is silent:
// with VRES = 552 the controller simply never drives the gates carrying image
// rows 276..299 (half the picture shifted down and a 24-row band left showing
// the previous frame), and a VRES larger than the controller's own 600-line
// space would just spend scan time addressing gates that do not exist.
//      image rows   0..299 -> gates 0, 2, 4, ..., 598   (highest: 598)
//      image rows 300..551 -> gates 1, 3, 5, ..., 503
static_assert(EPD398_VRES >= 599,
              "EPD398_VRES must reach gate 598 (image row 299); 552 leaves "
              "image rows 276..299 undriven");
static_assert(EPD398_VRES <= 600,
              "EPD398_VRES exceeds the controller's 600-line gate space");

// ---------------------------------------------------------------- pins
// Vendor wiring (screen side -> ESP32-C3 side):
//   DIN -> 6   SCK -> 4   DC -> 1   CS -> 7   RST -> 2   BUSY -> 10
#ifndef PIN_EPD_MOSI
#define PIN_EPD_MOSI 6
#endif
#ifndef PIN_EPD_SCK
#define PIN_EPD_SCK  4
#endif
#ifndef PIN_EPD_DC
#define PIN_EPD_DC   1
#endif
#ifndef PIN_EPD_CS
#define PIN_EPD_CS   7
#endif
#ifndef PIN_EPD_RST
#define PIN_EPD_RST  2
#endif
#ifndef PIN_EPD_BUSY
#define PIN_EPD_BUSY 10
#endif

// ---------------------------------------------------------------- timings
// BUSY polling timeout for the power-on / power-off handshakes (the vendor
// firmware uses 45000 ms).
#ifndef EPD398_BUSY_TIMEOUT_MS
#define EPD398_BUSY_TIMEOUT_MS 45000UL
#endif

// BUSY timeout for the display refresh itself. A 4-colour full refresh on this
// panel typically takes 15-25 s. The open-source driver allows 180 s, but that
// is three minutes of blocked main loop when the panel never answers, so 60 s is
// used here (still ~2.5x the expected refresh time).
#ifndef EPD398_REFRESH_TIMEOUT_MS
#define EPD398_REFRESH_TIMEOUT_MS 60000UL
#endif

// How long to wait for BUSY to release after a hardware reset. The panel
// normally answers within a few hundred ms, so 10 s is ample and keeps a dead
// panel from stalling boot.
#ifndef EPD398_RESET_WAIT_MS
#define EPD398_RESET_WAIT_MS 10000UL
#endif

// A BUSY release faster than this means "the waveform is not finished", so the
// driver waits EPD398_REFRESH_FALLBACK_MS instead of powering off. 0 disables
// the check.
#ifndef EPD398_BUSY_EARLY_MS
#define EPD398_BUSY_EARLY_MS 250UL
#endif

// The wait substituted when BUSY released early. MUST exceed the longest real
// refresh (15-25 s) or it powers the panel off mid-waveform and the last rows
// come out over-driven. The open-source driver's 14000 ms is too short.
#ifndef EPD398_REFRESH_FALLBACK_MS
#define EPD398_REFRESH_FALLBACK_MS 25000UL
#endif

// Settle time added after a normal-looking refresh before powering off.
#ifndef EPD398_REFRESH_SETTLE_MS
#define EPD398_REFRESH_SETTLE_MS 500UL
#endif

// Data byte for the 0x12 display-refresh command. 0x00 belongs with the
// open-source register sequence; the vendor sequence uses 0x01. Measured: this
// byte does not affect the darker band, but it does belong with the sequence.
#ifndef EPD398_REFRESH_MODE
#define EPD398_REFRESH_MODE 0x00
#endif

// Set to 0 to silence the driver's serial logging.
#ifndef EPD398_DEBUG
#define EPD398_DEBUG 1
#endif

// ---------------------------------------------------------------- colors
// 2bpp color codes. Codes 0x00/0x01 follow the open-source
// packMonoPixelGroup() convention; 0x02 is documented as yellow by the comment
// `if (color == 0x02) color = 0x01; // map yellow to red` in epd_driver.cpp.
// Verify with epd398TestPattern() on your panel.
#define EPD398_BLACK  0x00
#define EPD398_WHITE  0x01
#define EPD398_YELLOW 0x02
#define EPD398_RED    0x03

// ---------------------------------------------------------------- API
// Low level
void epd398GpioInit();
void epd398WaitBusy(unsigned long timeoutMs = EPD398_BUSY_TIMEOUT_MS);

// Same as epd398WaitBusy() but reports whether the panel actually released
// BUSY. Use this when the caller needs to distinguish a real refresh from a
// timeout (epd398Display2bpp() does).
bool epd398WaitBusyTimed(unsigned long timeoutMs);

void epd398Init();
void epd398PowerOff();
void epd398Sleep();

// Draw a 2bpp framebuffer (EPD398_COLOR_BYTES bytes) and refresh the panel.
// Full sequence: init -> upload -> 0x12 refresh -> power off.
void epd398Display2bpp(const uint8_t *colorBuf);

// Same, but without the init/power-off steps, for when you manage them yourself.
void epd398Upload2bpp(const uint8_t *colorBuf);

// True when the most recent epd398Display2bpp() completed its refresh (BUSY
// released, or the fallback wait ran). False after a BUSY timeout.
bool epd398LastOperationOk();

// ---------------------------------------------------------------- helpers
// Allocate the framebuffer (105984 bytes) on the heap. Returns nullptr on
// failure. Free with epd398FreeBuffer().
uint8_t *epd398AllocBuffer();

// Free a buffer obtained from epd398AllocBuffer().
void epd398FreeBuffer(uint8_t *buf);

// Fill the whole framebuffer with one color code.
void epd398Fill(uint8_t *buf, uint8_t color);

// Set one pixel. Coordinates outside the panel are ignored.
void epd398SetPixel(uint8_t *buf, int x, int y, uint8_t color);

// Fill a rectangle.
void epd398FillRect(uint8_t *buf, int x, int y, int w, int h, uint8_t color);

// Fill the framebuffer with vertical color bars (one per color) plus corner
// markers, to verify the color mapping on your panel.
void epd398TestPattern(uint8_t *buf, const uint8_t *colors, int count);

#endif // EPD_398_4COLOR_H
