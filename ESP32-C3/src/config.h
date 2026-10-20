#ifndef BADGE8_BLE_CONFIG_H
#define BADGE8_BLE_CONFIG_H

// ============================================================================
//  8adge BLE firmware - board and build constants
//
//  One board only: ESP32-C3-SuperMini + 3.98" 4-color e-paper (A1 panel,
//  768x552, 2bpp). There is no WiFi, no captive portal, no OTA and no
//  server-driven content in this build - the phone talks to the panel over
//  Bluetooth LE and nothing else. See docs/BLE_PROTOCOL.md.
//
//  The only thing written to flash is one 16-byte record: when the panel was
//  last refreshed, so the 180 s anti-ghosting cooldown survives a power cut.
//  See src/refresh_guard.{h,cpp}.
// ============================================================================

#include <Arduino.h>

// ---------------------------------------------------------------- panel pins
// Bit-banged 4-wire SPI, vendor wiring.
#define PIN_EPD_MOSI   6
#define PIN_EPD_SCK    4
#define PIN_EPD_CS     7
#define PIN_EPD_DC     1
#define PIN_EPD_RST    2
#define PIN_EPD_BUSY   10

// ---------------------------------------------------------------- buttons
// GPIO9 is the BOOT button and also an ESP32-C3 strapping pin, read with
// INPUT_PULLUP (pressed = LOW). Only used for two conveniences, neither is
// required for the BLE feature to work:
//   short press           redraw the "connect over Bluetooth" screen
//   hold CFG_BTN_REBOOT_MS  reboot the device
#define PIN_CFG_BTN        9
#define CFG_BTN_REBOOT_MS  8000UL
#define BTN_DEBOUNCE_MS    40UL

// ---------------------------------------------------------------- status LED
// The ONBOARD blue LED of the C3-SuperMini sits on GPIO8 and is ACTIVE LOW.
// GPIO8 is a strapping pin sampled at reset, so ledInit() parks it OFF.
#define PIN_LED            8
#define PIN_LED_ACTIVE_LOW 1

// ---------------------------------------------------------------- pin sanity
// Compile time, because a mistyped number here is otherwise invisible until the
// panel stays blank or the LED never lights. GPIO8 (LED) and GPIO9 (BOOT) are
// the board's strapping pins and are not used by the panel; GPIO18/19 are the
// C3's USB D-/D+, which this build needs for its CDC console.
constexpr bool pinIn(const int *p, int n, int pin) {
    return n <= 0 ? false : (p[0] == pin || pinIn(p + 1, n - 1, pin));
}

constexpr bool pinsUnique(const int *p, int n) {
    return n <= 1 ? true : (!pinIn(p + 1, n - 1, p[0]) && pinsUnique(p + 1, n - 1));
}

constexpr int kUsedPins[] = {
    PIN_EPD_MOSI, PIN_EPD_SCK, PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY,
    PIN_CFG_BTN, PIN_LED,
};
static_assert(pinsUnique(kUsedPins, 8), "two of the board's functions share a pin");
static_assert(!pinIn(kUsedPins, 6, 8) && !pinIn(kUsedPins, 6, 9),
              "the panel must not claim a strapping pin (GPIO8 LED / GPIO9 BOOT)");
static_assert(!pinIn(kUsedPins, 8, 18) && !pinIn(kUsedPins, 8, 19),
              "GPIO18/19 are USB D-/D+ and must stay free for the console");

// ---------------------------------------------------------------- geometry
// The panel itself is a 768x552 landscape part, 2bpp.
//
// The numbers live ONCE, in epd_398_4color.h (EPD398_WIDTH/HEIGHT/BPP), which is
// also what the driver and ble_geometry.h compile against. There used to be a
// second copy here (EPD_WIDTH/EPD_HEIGHT/EPD_BPP) plus a third one as -D flags in
// platformio.ini; the copies disagreed with nothing but each other, which is
// exactly the kind of drift that only shows up as a broken image on the glass.
// The canvas/panel relation is now checked at compile time in ble_geometry.h.

// ---------------------------------------------------------------- orientation
// Everything the phone sends and everything this firmware draws is expressed in
// a 552x768 portrait *canvas*, and the device rotates it onto the landscape
// panel as the last step (see ble_geometry.h). A portrait photo frame is the
// point of this build, so the canvas is the natural working space for the app:
// it packs its picture top-to-bottom exactly as it is on screen.
//
// This is the clockwise rotation applied to the canvas on its way to the panel:
//
//   90   canvas top-left lands at the panel's top-right   (default)
//   270  canvas top-left lands at the panel's bottom-left (180 degrees from 90)
//
// 180 is not offered on purpose: a 180 degree turn would leave the bitmap
// 552x768, which cannot be laid over a 768x552 panel. Portrait on this glass is
// always +/-90, and the two values above are the "upright or upside-down" pair.
//
// If a flashed device comes out rotated the wrong way - because its panel sits
// the other way round inside the frame - flip this between 90 and 270 and
// reflash. The hint screen and phone images both follow it, so they can never
// disagree.
//
// (Making the frame landscape after all would need BLE_IMAGE_WIDTH/HEIGHT in
// ble_protocol.h changed to 768x552 as well, and the app rebuilt - the canvas
// size is what the phone packs against.)
#define BLE_ROTATION 90

// ---------------------------------------------------------------- hint screen
// The idle screen is a four-colour border with the instructions in the middle.
// Canvas size is 552x768.
//
//   HINT_RING_THICKNESS  border band width in pixels
//   HINT_RING_BLOCKS     how many colour blocks the ring is divided into, all
//                        the way round. The colours cycle black -> white -> red
//                        -> yellow, so a multiple of 4 ends the loop cleanly;
//                        24 blocks is 6 complete cycles.
//   HINT_RING_KEYLINE    a thin black line drawn just inside the ring. Without
//                        it the white blocks of the ring are indistinguishable
//                        from the white interior and the border looks broken.
//                        Set to 0 to drop it.
#define HINT_RING_THICKNESS 24
#define HINT_RING_BLOCKS    24
#define HINT_RING_KEYLINE   3

// ---------------------------------------------------------------- BLE
// Advertised name: "8adge-" + the last 5 hex digits of the WiFi station MAC,
// in the order esp_read_mac() fills them. The suffix is stable per board, so a
// phone that has seen the device before recognises the same name again; the App
// must not parse it, GET_INFO reports the identity fields instead.
#define BLE_NAME_PREFIX   "8adge-"
#define BLE_NAME_SUFFIX_LEN 5

// Preferred ATT MTU. 517 is the maximum an Android central will negotiate; the
// usable payload per write is MTU-3. Smaller centrals simply get smaller
// chunks - the protocol reports the real value in GET_INFO / READY.
#define BLE_MTU_PREFERRED 517

// Largest DATA write the firmware is prepared to accept, whatever the MTU says.
//
// 512 is not a tuning choice, it is NimBLE's BLE_ATT_ATTR_MAX_LEN: a
// characteristic's value buffer defaults to that size and the host rejects any
// write longer than it. With an MTU of 517 the ATT payload would be 514 bytes,
// which would simply come back as an ATT error.
#define BLE_DATA_CHUNK_MAX 512

// ---------------------------------------------------------------- throughput
//
// Measured on real hardware with the stock settings: ~9.5 kB/s, i.e. about one
// 512-byte write per connection event at a ~48 ms interval (Android's default
// when the connection is not prioritised). The transfer itself is not the
// bottleneck - the link is idle most of the time. Three things fix that, and
// only the first two are the firmware's job:
//
//  1. Ask for a short connection interval + zero latency (below). At 15 ms the
//     same one-write-per-event gives ~34 kB/s; the phone may clamp the request.
//  2. Ask for data length extension (below) so one ATT write is not chopped
//     into twenty 27-byte link-layer PDUs.
//  3. The app must call requestConnectionPriority(CONNECTION_PRIORITY_HIGH) and
//     pipeline its writes instead of waiting for a callback after every chunk.
//     Documented in docs/BLE_PROTOCOL.md - the firmware cannot do it for it.
//
// Connection interval in units of 1.25 ms: 6 = 7.5 ms, 12 = 15 ms.
// Android may grant a wider interval than requested; 7.5-15 ms is the bracket
// it uses for its own "high priority" connections, so it usually agrees.
#define BLE_CONN_MIN_INTERVAL 6
#define BLE_CONN_MAX_INTERVAL 12
// Connection latency in events (0 = the device answers in every event).
#define BLE_CONN_LATENCY 0
// Supervision timeout in units of 10 ms. Must satisfy
// timeout*10 > (1+latency)*maxInterval*1.25*2, i.e. > 37.5 ms here; 2 s is
// comfortably above and still recovers from a phone walking out of range.
#define BLE_CONN_TIMEOUT 200

// Data length extension: the largest link-layer payload we ask for. 251 is the
// BLE maximum (a 4.2 feature). Without it the controller may stay at the 27-byte
// default, and every 512-byte ATT write costs ~20 LL packets.
#define BLE_LL_TX_OCTETS 251

// The numbers above have relationships the specification (and NimBLE) enforce at
// run time, which used to live only in prose. They are cheap to check here, and
// getting one wrong shows up as a link that drops during a transfer or a write
// the host silently refuses.
//   * supervision timeout (10 ms units) must exceed
//     (1 + latency) * maxInterval * 1.25 ms * 2  -> multiply by 20 to stay
//     integral: timeout*20 > (1+latency)*maxInterval*5
//   * the DATA chunk ceiling is NimBLE's characteristic value buffer
//     (BLE_ATT_ATTR_MAX_LEN = 512): a longer write comes back as an ATT error
static_assert((int)BLE_CONN_TIMEOUT * 20 > (1 + (int)BLE_CONN_LATENCY) * (int)BLE_CONN_MAX_INTERVAL * 5,
              "BLE_CONN_TIMEOUT is too short for the requested interval/latency");
static_assert(BLE_DATA_CHUNK_MAX <= 512,
              "BLE_DATA_CHUNK_MAX exceeds NimBLE's 512-byte characteristic value buffer");
static_assert(BLE_CONN_MIN_INTERVAL <= BLE_CONN_MAX_INTERVAL,
              "the connection interval range is inverted");

// How many DATA chunks the device reports progress after. This is a *progress
// report*, NOT a stop-and-wait barrier: the app may keep sending while an ACK is
// in flight. The link layer already guarantees delivery and flow control (every
// LL packet is acknowledged or retransmitted, and the controller will not accept
// more than it can buffer), so an app that waits for these ACKs is slowing
// itself down for no reason. 32 chunks = 16 KB = ~6 reports per image.
#define BLE_WINDOW_CHUNKS 32

// How long the device waits for the next DATA byte (or a RETRY_BLOCK /
// BLOCK_CRCS frame) before abandoning the session and returning to CONNECTED.
// A complete 2bpp image takes ~11 s at the measured 9.5 kB/s, and the gap
// between two 512-byte writes is tens of milliseconds, so 15 s of silence can
// only mean the app stalled. Without this a killed app that left the link up
// would park the device in RECEIVING and reject every later BEGIN_IMAGE with
// ERR_STATE until it disconnected.
#define BLE_RX_TIMEOUT_MS 15000UL

// Shortest interval between two high-frequency log lines from the receive path
// (a repeated/dropped chunk, a block that keeps failing CRC). Serial.printf
// runs on the NimBLE host task with ARDUINO_USB_CDC_ON_BOOT=1, and the USB CDC
// write path can block while a host is attached but not draining, so a busy or
// broken app must not be able to turn the console into a stall.
#define BLE_LOG_THROTTLE_MS 300UL

// Grace period between BEGIN_IMAGE and the first DATA byte. An app may need a
// while to quantise/compress the image after it has already announced the
// transfer; the 15 s data timeout must not expire before the first byte has
// even been sent.
#define BLE_BEGIN_TIMEOUT_MS 60000UL

// A user confirmation may override the cooldown once, but a forced refresh
// still has to be at least this far from the previous refresh. Without a floor
// a "confirm" loop could repaint twice within a few seconds. After a forced
// refresh, the next *forced* one requires the full BLE_REFRESH_MIN_GAP_MS
// (see refresh_guard.cpp) - the effective worst case is one refresh per
// cooldown, not one per confirmation dialog.
#define BLE_REFRESH_FORCED_MIN_GAP_MS 60000UL

// The app-supplied clock is untrusted. Within one power cycle the adopted time
// may never run further ahead than the device's own uptime plus this
// tolerance, so repeated SYNC_TIME frames cannot "fast-forward" the cooldown.
// (The first sync after a power-up cannot be validated at all - there is no
// trusted time source on this board - and is documented as a trust boundary.)
#define BLE_CLOCK_MAX_FORWARD_SKEW_S 30

// ---------------------------------------------------------------- integrity
// Image verification is done per *block* rather than once for the whole image,
// so a corrupted or dropped region costs one block to resend instead of the
// whole 106 KB. Block size is 1 << BLE_BLOCK_SHIFT; 12 gives 4 KB, i.e. 26
// blocks for a full 2bpp frame.
//
// The app declares the per-block CRCs up front (BLOCK_CRCS), and END_IMAGE
// reports a bitmap of the blocks that failed to verify.
#define BLE_BLOCK_SHIFT_DEFAULT 12     // 4096-byte blocks
#define BLE_BLOCK_SHIFT_MIN 11         // 2 KB
#define BLE_BLOCK_SHIFT_MAX 14         // 16 KB
// Ceiling on the block count, i.e. the size of the failure bitmap the
// END_IMAGE response can carry: 128 blocks = 16 bytes. 105984 / 1024 = 104, so
// even 1 KB blocks would fit.
#define BLE_MAX_BLOCKS 128

// ---------------------------------------------------------------- refresh cooldown
// Minimum interval between two full panel refreshes. This is not a nicety, it
// is a panel-lifetime limit: a 4-colour refresh drives a 15-25 s bipolar AC
// waveform that physically moves red/yellow/black/white pigment particles
// through the microcapsule fluid, the coloured pigments have low mobility, and
// starting the next refresh before they have settled lets particles of opposite
// polarity clump together. That damage - colour contamination, red-tinted
// white, grey ghosting - is irreversible, no matter how many "refresh to white"
// cycles follow. Vendors specify >= 180 s for 4-colour parts, and most disable
// partial refresh in the controller outright; this firmware has no partial
// refresh path either.
//
// Enforced by src/refresh_guard.{h,cpp}, which also explains how the interval is
// measured on a device with no clock of its own. The app can ask to override it,
// but only after showing the user a warning: see docs/BLE_PROTOCOL.md.
#define BLE_REFRESH_MIN_GAP_MS 180000UL   // 180 s, the vendor minimum

// How long a user confirmation stays valid. It has to outlast one image
// transfer (a couple of seconds over a good link, worse over a bad one) but must
// not linger, or a confirmation the user gave for one refresh would silently
// authorise the next one.
#define BLE_REFRESH_GRANT_MS 120000UL

// Firmware version reported in the GET_INFO reply. Keep in sync with the doc.
// 1.1.0: protocol v2 - per-block CRC with selective retransmission, fast
// connection parameters, data length extension, non-blocking progress reports.
// 1.2.0: protocol v3 - full-refresh cooldown (SYNC_TIME / CONFIRM_REFRESH /
// EVT_REFRESH_BLOCKED), and the one thing this build writes to flash.
// 1.2.1: fix 1bpp END_IMAGE CRC (format selector was stored in a bool, so
// BLE_FMT_1BPP collapsed to BLE_FMT_2BPP), and add the stalled-transfer timeout.
// 1.3.0: audit fixes - hint paint claims the framebuffer, cross-task critical
// sections, untrusted-clock clamping and a forced-refresh floor, MTU validation,
// BEGIN/END field validation, conservative NVS records, rollover-safe LED hold.
// 1.3.1: second audit - END_IMAGE success reply is the documented 8 bytes again
// (it sent 26, 18 of them uninitialised stack), over-length DATA is ignored
// instead of discarding a verified image, an END_IMAGE with no session answers
// ERR_STATE + a data_hint byte, a failed END_IMAGE re-emits RECEIVING, PING
// echoes are MTU-gated, RETRY_BLOCK validates its reserved field, and the
// confirmation floor is measured against the real interval when a clock is known.
#define FW_VERSION_MAJOR 1
#define FW_VERSION_MINOR 3
#define FW_VERSION_PATCH 1

// ---------------------------------------------------------------- behaviour
// Power-on leaves the panel alone (e-ink keeps the previous picture). If no
// central has connected by the time this elapses, the "connect over Bluetooth"
// screen is painted instead. Once an image has actually been transferred the
// hint is never drawn again in that power cycle, so a finished picture is not
// overwritten two minutes later.
#define BLE_HINT_DELAY_MS (2UL * 60UL * 1000UL)

// Serial console. Set to 0 to compile the logging out.
#ifndef BLE_DEBUG
#define BLE_DEBUG 1
#endif

#if BLE_DEBUG
#define BLE_LOG(...) Serial.printf(__VA_ARGS__)
#else
#define BLE_LOG(...) do {} while (0)
#endif

#endif // BADGE8_BLE_CONFIG_H
