// ============================================================================
//  3.98" 4-color e-paper driver - A1 panel (768x552, black/white/red/yellow).
//
//  The panel's gate lines are wired in two interleaved groups, so image row r
//  lives at gate address 2*r (r <= 299) or 2*r - 599 (r >= 300) inside a 600-line
//  controller address space. 0x61 must therefore report VRES = 600. With 552 the
//  controller cannot reach the gates carrying image rows 276..299 and the picture
//  comes out cut in half at the middle. See the header for the full reasoning.
//
//  The register sequence is the open-source JD79665 one from
//  github.com/krstc/8adge_adapt_HUAWEI_eink (MIT), which is what removes the
//  darker band the vendor firmware's "LUT from OTP" sequence produced across
//  image rows 252..299. Its reset and its 0x12 <- 0x00 are kept; its VRES = 552
//  is NOT (that is the value that leaves 24 rows undriven).
//
//  This file is the single-path final version: no build-time switches, because
//  every alternative was measured and is worse (see the header for the list).
// ============================================================================

// When built inside the 8adge project, pick up the board pin definitions
// from config.h so the driver follows the project's wiring macros instead of
// the built-in defaults.
#if defined(__has_include)
#  if __has_include("config.h")
#    include "config.h"
#  endif
// The status LED lives in led.h in the 8adge project. Guarded like config.h
// so this driver can still be dropped into a standalone sketch, where there is
// no LED module and the waits fall back to a plain delay().
#  if __has_include("led.h")
#    include "led.h"
#    define EPD398_HAS_LED 1
#  endif
#endif

#ifndef EPD398_HAS_LED
#  define EPD398_HAS_LED 0
#endif

#include "epd_398_4color.h"

// Wait helper for the long panel waits. A refresh holds the caller for tens of
// seconds; with the LED module present this keeps the status LED blinking
// through it instead of freezing on whatever state it was left in.
static void epd398WaitTick(unsigned long ms) {
#if EPD398_HAS_LED
    ledDelay(ms);
#else
    delay(ms);
#endif
}

#if EPD398_DEBUG
#define EPD398_LOG(...) Serial.printf(__VA_ARGS__)
#else
#define EPD398_LOG(...) do {} while (0)
#endif

// Result of the most recent full display operation, so callers can tell a real
// refresh from a panel that never answered. See epd398LastOperationOk().
static bool s_lastOperationOk = false;

// The pins are configured on first use, so this driver is safe to call without
// the host doing epd398GpioInit() first (the 8adge project delays that until
// the first paint on purpose). ensureGpio() is what every public entry point
// calls; it is only a pin configuration, so it never disturbs the panel.
static bool s_gpioReady = false;
static void ensureGpio();

// ---------------------------------------------------------------- SPI layer
// Bit-banged 4-wire SPI. MSB first, MOSI is set up before SCK rises, SCK idles
// low. Verified against the vendor's spiWriteByte at 0x42000FF4.
static void epd398SpiWriteByte(uint8_t data) {
    for (int i = 0; i < 8; i++) {
        digitalWrite(PIN_EPD_MOSI, (data >> 7) & 0x01);
        digitalWrite(PIN_EPD_SCK, HIGH);
        data = (uint8_t)(data << 1);
        digitalWrite(PIN_EPD_SCK, LOW);
    }
}

static void epd398SendCommand(uint8_t cmd) {
    digitalWrite(PIN_EPD_DC, LOW);      // DC low  = command
    digitalWrite(PIN_EPD_CS, LOW);
    epd398SpiWriteByte(cmd);
    digitalWrite(PIN_EPD_CS, HIGH);
}

static void epd398SendData(uint8_t dat) {
    digitalWrite(PIN_EPD_DC, HIGH);     // DC high = data
    digitalWrite(PIN_EPD_CS, LOW);
    epd398SpiWriteByte(dat);
    digitalWrite(PIN_EPD_CS, HIGH);
}

// ---------------------------------------------------------------- BUSY wait
// BUSY is active LOW: wait until it goes HIGH. Returns true when the panel
// released BUSY, false on timeout.
//
// Uses epd398WaitTick() rather than delay() so the status LED keeps blinking
// while the panel is busy - a refresh holds this loop for tens of seconds.
bool epd398WaitBusyTimed(unsigned long timeoutMs) {
    ensureGpio();
    unsigned long t0 = millis();
    while (digitalRead(PIN_EPD_BUSY) == LOW) {
        epd398WaitTick(10);
        if (millis() - t0 > timeoutMs) {
            EPD398_LOG("[EPD398] BUSY TIMEOUT after %lums\n", timeoutMs);
            return false;
        }
    }
    return true;
}

void epd398WaitBusy(unsigned long timeoutMs) {
    (void)epd398WaitBusyTimed(timeoutMs);
}

// ---------------------------------------------------------------- GPIO init
void epd398GpioInit() {
    pinMode(PIN_EPD_BUSY, INPUT);
    pinMode(PIN_EPD_RST,  OUTPUT);
    pinMode(PIN_EPD_DC,   OUTPUT);
    pinMode(PIN_EPD_CS,   OUTPUT);
    pinMode(PIN_EPD_SCK,  OUTPUT);
    pinMode(PIN_EPD_MOSI, OUTPUT);
    digitalWrite(PIN_EPD_RST, HIGH);
    digitalWrite(PIN_EPD_CS,  HIGH);
    digitalWrite(PIN_EPD_SCK, LOW);
    s_gpioReady = true;
}

// Configure the pins if the host has not (or has not yet). EVERY public entry
// point calls this: the header promises a driver that is safe to use on its own,
// and epd398Init()/epd398Upload2bpp() write RST/DC/CS immediately. It only sets
// pin directions (RST is left HIGH), so it cannot disturb the panel - which is
// what lets the host defer it to the first paint.
static void ensureGpio() {
    if (!s_gpioReady) epd398GpioInit();
}

// ---------------------------------------------------------------- window
// Command 0x83 takes 9 data bytes:
//   x_start(2) x_end(2) y_start(2) y_end(2) flag
// Only the full-screen window is used: one 0x10 burst writes the whole frame, and
// the controller walks its 600-line gate space in the interleaved order on its
// own, so no per-row windows are needed.
//
// Careful reading of the two statements above, because they look contradictory:
// the "2*r / 2*r-599" table in the header says where a *panel* row's gate sits in
// the controller's address space, while the burst writes *image* rows one after
// another through this window. They are consistent only because the controller's
// own line counter for a 0x10 burst already walks the two interleaved groups in
// the order the framebuffer holds them - which is exactly why VRES must be 600
// here (a 552-line space would stop driving the gates of image rows 276..299) and
// why replacing this with 552 single-row 0x83 windows or VRES = 552 reproduces
// the half-picture/stretched failure modes listed in the header. This mapping was
// measured on the A1 panel; if the register sequence is ever changed, re-measure
// before trusting it.
static void epd398SetWindow(uint16_t y0, uint16_t y1) {
    epd398SendCommand(0x83);
    epd398SendData(0x00); epd398SendData(0x00);          // x start = 0
    epd398SendData(0x02); epd398SendData(0xFF);          // x end   = 767
    epd398SendData((uint8_t)(y0 >> 8)); epd398SendData((uint8_t)y0);
    epd398SendData((uint8_t)(y1 >> 8)); epd398SendData((uint8_t)y1);
    epd398SendData(0x01);
}

// Restore the full controller window (0..767 x 0..VRES-1). The 0x10 burst is
// written through this window.
static void epd398SetFullWindow() {
    epd398SetWindow(0, EPD398_VRES - 1);
}

// ---------------------------------------------------------------- init
// 0x61 resolution setting. VRES is the controller's gate address space, which is
// 600 - not the panel height (552). See the addressing note in the header: a
// 552-line space cannot reach the gates that carry image rows 276..299.
static void epd398SendResolution() {
    epd398SendCommand(0x61);                 // resolution setting
    epd398SendData((uint8_t)(EPD398_WIDTH >> 8));
    epd398SendData((uint8_t)(EPD398_WIDTH & 0xFF));    // HRES = 0x0300 = 768
    epd398SendData((uint8_t)(EPD398_VRES >> 8));
    epd398SendData((uint8_t)(EPD398_VRES & 0xFF));     // VRES = 0x0258 = 600
}

void epd398Init() {
    ensureGpio();
    // ---- JD79665 register sequence (krstc/8adge_adapt_HUAWEI_eink) ----
    // Copied from the reference driver's EPD_PANEL_38_JD79665_BWRY branch, with
    // one change: 0x61 gets VRES = 600 instead of the reference's 552, because
    // the reference's own value leaves image rows 276..299 undriven on this
    // panel. Everything else is reproduced as the reference has it - including
    // the reset (line high, then a 2 ms LOW pulse), which differs from the
    // vendor firmware's opposite order with much longer delays.
    digitalWrite(PIN_EPD_RST, HIGH); delay(20);
    digitalWrite(PIN_EPD_RST, LOW);  delay(2);
    digitalWrite(PIN_EPD_RST, HIGH); delay(20);

    epd398WaitBusy(EPD398_RESET_WAIT_MS);
    delay(30);

    epd398SendCommand(0xAA);                 // CMDH / panel-specific setup
    epd398SendData(0x49);
    epd398SendData(0x55);
    epd398SendData(0x20);
    epd398SendData(0x08);
    epd398SendData(0x09);
    epd398SendData(0x18);

    epd398SendCommand(0x01);                 // power setting
    epd398SendData(0x3F);

    epd398SendCommand(0x00);                 // panel setting (PSR)
    epd398SendData(0x4B);
    epd398SendData(0x69);

    epd398SendCommand(0x05);                 // booster soft start
    epd398SendData(0x40);
    epd398SendData(0x1F);
    epd398SendData(0x1F);
    epd398SendData(0x2C);

    epd398SendCommand(0x08);                 // VCOM DC setting
    epd398SendData(0x6F);
    epd398SendData(0x1F);
    epd398SendData(0x1F);
    epd398SendData(0x22);

    epd398SendCommand(0x06);                 // TSSET
    epd398SendData(0x6F);
    epd398SendData(0x1F);
    epd398SendData(0x1F);
    epd398SendData(0x14);
    epd398SendData(0x14);

    epd398SendCommand(0x03);                 // VCOM / data interval (VDCS)
    epd398SendData(0x00);
    epd398SendData(0x54);
    epd398SendData(0x00);
    epd398SendData(0x44);

    epd398SendCommand(0x60);                 // TCON setting
    epd398SendData(0x02);
    epd398SendData(0x00);

    epd398SendCommand(0x30);                 // PLL control
    epd398SendData(0x08);

    epd398SendCommand(0x50);                 // VCOM and data interval setting
    epd398SendData(0x3F);

    epd398SendResolution();                  // 768 x 600 (reference: 768 x 552)

    epd398SendCommand(0x65);                 // GSST
    epd398SendData(0x10);
    epd398SendData(0x00);
    epd398SendData(0x20);
    epd398SendData(0x00);

    epd398SendCommand(0xE3);                 // power saving
    epd398SendData(0x2F);

    epd398SendCommand(0x84);                 // VCOM and data interval (2nd)
    epd398SendData(0x01);

    epd398SetFullWindow();                   // 0x83, 0..767 x 0..599
    epd398SendCommand(0x04);                 // power ON
    epd398WaitBusy(EPD398_BUSY_TIMEOUT_MS);

    EPD398_LOG("[EPD398] init done (VRES=%d, BUSY=%d)\n",
               (int)EPD398_VRES, digitalRead(PIN_EPD_BUSY));
}

// ---------------------------------------------------------------- upload
// 2bpp framebuffer -> controller RAM, as one 0x10 burst through the full-screen
// window. The controller walks its 600-line gate space in the interleaved order,
// so framebuffer order (image rows 0..551) lands on the right gates without the
// driver issuing 552 single-row 0x83 windows.
void epd398Upload2bpp(const uint8_t *colorBuf) {
    if (!colorBuf) {
        EPD398_LOG("[EPD398] upload skipped: null framebuffer\n");
        return;
    }
    ensureGpio();

    epd398SendCommand(0x04);      // power ON
    epd398WaitBusy(EPD398_BUSY_TIMEOUT_MS);

    epd398SetFullWindow();
    epd398SendCommand(0x10);              // data start transmission
    for (int i = 0; i < EPD398_COLOR_BYTES; i++) {
        epd398SendData(colorBuf[i]);
    }
}

// ---------------------------------------------------------------- power
void epd398PowerOff() {
    ensureGpio();
    epd398SendCommand(0x02);
    epd398SendData(0x00);
    epd398WaitBusy(EPD398_BUSY_TIMEOUT_MS);
}

void epd398Sleep() {
    ensureGpio();
    epd398SendCommand(0x07);
    epd398SendData(0xA5);
    delay(200);
}

// ---------------------------------------------------------------- refresh
// Starts the display update and waits for it to actually finish.
//
// The important detail: BUSY on this panel can return HIGH well before the
// waveform has completed. Trusting it alone means powering the panel off in the
// middle of a refresh, which leaves the screen blank. A release in under
// EPD398_BUSY_EARLY_MS is therefore treated as "not finished" and replaced by a
// fixed EPD398_REFRESH_FALLBACK_MS wait.
static bool epd398RefreshPanel() {
    // Re-arm the full window, as the reference driver does before its refresh.
    // epd398Upload2bpp() already left it at 0..599, so this is a no-op in
    // practice - kept because the sequence is otherwise reproduced verbatim.
    epd398SetFullWindow();

    epd398SendCommand(0x12);                 // display refresh
    epd398SendData(EPD398_REFRESH_MODE);

    EPD398_LOG("[EPD398] refresh 0x12 <- 0x%02X\n", (unsigned)EPD398_REFRESH_MODE);

    unsigned long t0 = millis();
    bool busyOk = epd398WaitBusyTimed(EPD398_REFRESH_TIMEOUT_MS);
    unsigned long elapsed = millis() - t0;

    if (!busyOk) {
        EPD398_LOG("[EPD398] refresh BUSY timeout after %lums\n", elapsed);
        return false;
    }
    if (elapsed < EPD398_BUSY_EARLY_MS) {
        // BUSY came back far too quickly for a real refresh: wait it out.
        EPD398_LOG("[EPD398] BUSY only %lums -> fallback wait %lums\n",
                   elapsed, (unsigned long)EPD398_REFRESH_FALLBACK_MS);
        epd398WaitTick(EPD398_REFRESH_FALLBACK_MS);
    } else {
        epd398WaitTick(EPD398_REFRESH_SETTLE_MS);
    }
    return true;
}

// ---------------------------------------------------------------- top level
void epd398Display2bpp(const uint8_t *colorBuf) {
    if (!colorBuf) {
        EPD398_LOG("[EPD398] display skipped: null framebuffer\n");
        s_lastOperationOk = false;
        return;
    }
    ensureGpio();

    unsigned long t0 = millis();
    epd398Init();
    EPD398_LOG("[EPD398] init %lums\n", millis() - t0);

    epd398Upload2bpp(colorBuf);
    EPD398_LOG("[EPD398] data done %lums\n", millis() - t0);

    bool ok = epd398RefreshPanel();
    EPD398_LOG("[EPD398] refresh done %lums (ok=%d)\n", millis() - t0, ok ? 1 : 0);

    epd398PowerOff();
    EPD398_LOG("[EPD398] all done %lums\n", millis() - t0);

    s_lastOperationOk = ok;
}

bool epd398LastOperationOk() {
    return s_lastOperationOk;
}

// ============================================================================
//  Framebuffer helpers
// ============================================================================

uint8_t *epd398AllocBuffer() {
    uint8_t *buf = (uint8_t *)malloc(EPD398_COLOR_BYTES);
    if (buf) {
        EPD398_LOG("[EPD398] buffer %d bytes allocated\n", EPD398_COLOR_BYTES);
    } else {
        EPD398_LOG("[EPD398] buffer allocation FAILED (%d bytes)\n", EPD398_COLOR_BYTES);
    }
    return buf;
}

void epd398FreeBuffer(uint8_t *buf) {
    if (buf) {
        free(buf);
    }
}

// 2bpp packing: 4 pixels per byte, most significant pair = leftmost pixel.
void epd398Fill(uint8_t *buf, uint8_t color) {
    if (!buf) {
        return;
    }
    uint8_t packed = (uint8_t)((color & 0x03) * 0x55);   // 0b00/01/10/11 repeated
    memset(buf, packed, EPD398_COLOR_BYTES);
}

void epd398SetPixel(uint8_t *buf, int x, int y, uint8_t color) {
    if (!buf || x < 0 || x >= EPD398_WIDTH || y < 0 || y >= EPD398_HEIGHT) {
        return;
    }
    int idx = y * EPD398_ROW_BYTES + (x >> 2);
    int shift = 6 - ((x & 0x03) * 2);
    buf[idx] = (uint8_t)((buf[idx] & ~(0x03 << shift)) | ((color & 0x03) << shift));
}

void epd398FillRect(uint8_t *buf, int x, int y, int w, int h, uint8_t color) {
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            epd398SetPixel(buf, i, j, color);
        }
    }
}

void epd398TestPattern(uint8_t *buf, const uint8_t *colors, int count) {
    if (!buf || !colors || count <= 0) {
        return;
    }
    epd398Fill(buf, EPD398_WHITE);

    int barW = EPD398_WIDTH / count;
    for (int c = 0; c < count; c++) {
        int x0 = c * barW;
        int x1 = (c == count - 1) ? EPD398_WIDTH : x0 + barW;
        epd398FillRect(buf, x0, 0, x1 - x0, EPD398_HEIGHT, colors[c]);
    }

    // corner markers: a 24x24 black square in each corner, to expose a wrong
    // row mapping or a mirrored/rotated panel immediately
    epd398FillRect(buf, 0, 0, 24, 24, EPD398_BLACK);
    epd398FillRect(buf, EPD398_WIDTH - 24, 0, 24, 24, EPD398_BLACK);
    epd398FillRect(buf, 0, EPD398_HEIGHT - 24, 24, 24, EPD398_BLACK);
    epd398FillRect(buf, EPD398_WIDTH - 24, EPD398_HEIGHT - 24, 24, 24, EPD398_BLACK);

    // horizontal ruler line every 50 rows
    for (int y = 50; y < EPD398_HEIGHT; y += 50) {
        epd398FillRect(buf, 0, y, EPD398_WIDTH, 1, EPD398_BLACK);
    }
}
