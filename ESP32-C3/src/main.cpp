// ============================================================================
//  8adge BLE firmware - ESP32-C3-SuperMini + 3.98" 4-color e-paper.
//
//  This build has no WiFi stack at all. There is no captive portal, no HTTP
//  client, no OTA, no server-driven content and no network code of any kind -
//  the only way in is Bluetooth LE, and the only thing the phone can do is send
//  a picture. See docs/BLE_PROTOCOL.md for the wire format.
//
//  Behaviour:
//
//    1. Power on advertises "8adge-XXXXX" (the last 5 hex digits of the
//       station MAC) and does NOT touch the panel - e-ink keeps whatever was on
//       it.
//    2. If nothing connects within BLE_HINT_DELAY_MS (2 minutes), the panel is
//       repainted with the device name and "请使用蓝牙连接：".
//    3. A verified image transfer repaints the panel with the picture. The
//       automatic idle hint is then suppressed for the rest of the power cycle,
//       but an explicit request (BOOT short press, or the app's SHOW_HINT) may
//       still repaint the hint over it - that is always user/app initiated.
//    4. Every one of those repaints owes the panel a cool-down: a 4-colour
//       refresh drives a 15-25 s AC waveform and repeating it too soon damages
//       the panel permanently. src/refresh_guard.{h,cpp} owns that rule; this
//       file just asks it before painting and reports back afterwards. With no
//       clock (nobody has connected yet) the first hint of a power cycle can
//       therefore be up to BLE_REFRESH_MIN_GAP_MS of uptime late, which is what
//       makes the rule provable rather than merely probable.
//    5. Both paint paths claim the one framebuffer before drawing: a hint must
//       not race a phone streaming an image into the same bytes.
//
//  Everything is event driven from the BLE callbacks, so loop() is a thin
//  poller: no state machine, no timers beyond the one idle deadline.
// ============================================================================

#include <Arduino.h>
#include <string.h>

#include "esp_mac.h"
#include "esp_system.h"

#include "ble_protocol.h"
#include "ble_screen.h"
#include "ble_service.h"
#include "config.h"
#include "epd_398_4color.h"
#include "led.h"
#include "refresh_guard.h"

// ---------------------------------------------------------------- state

static uint8_t *s_frame = nullptr;       // the one and only 105984-byte buffer
static char s_deviceName[32] = {0};

static bool s_gpioReady = false;         // epd398GpioInit() runs on first use
static bool s_imageShown = false;        // a picture from the phone is up
static bool s_hintShown = false;         // the automatic hint already ran
static bool s_hintWanted = false;        // a hint was asked for but the panel
static const char *s_hintWhy = nullptr;  //   is still inside its cool-down
static uint8_t s_hintOpcode = 0;         // refused command, so only that
                                         //   command's grant can admit it
static bool s_hintAuto = false;          // the pending hint came from the idle
                                         //   deadline, i.e. it asked for "nobody
                                         //   is connected" which can expire
static unsigned long s_idleSince = 0;    // when the link last went idle
static bool s_wasConnected = false;
static bool s_errorHold = false;         // LED error state hold
static unsigned long s_errorSince = 0;

static unsigned long s_btnDownAt = 0;
static bool s_btnDown = false;
static bool s_btnFired = false;

// ---------------------------------------------------------------- helpers

// "8adge-" + the last 5 hex digits of the station MAC, so the name is stable
// per board and the App sees the same device every time.
//
// Byte order matters here and is easy to get wrong:
//   * esp_read_mac() fills the array big-endian - mac[0] is the first octet,
//     the order a MAC is normally printed in;
//   * ESP.getEfuseMac() returns a uint64_t whose *lowest* byte is the first
//     octet, so it has to be read out least-significant byte first.
// Mixing the two up does not fail loudly: it yields a reversed address that
// still looks like a perfectly plausible name - "8adge-2DA90" instead of
// "8adge-8D134" for MAC 90:DA:72:88:D1:34.
static void buildDeviceName() {
    uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
    bool fromEfuse = false;

    // NOTE: ESP_MAC_WIFI_STA is an enum value, not a macro. Testing it with
    // `#if defined(ESP_MAC_WIFI_STA)` is always false and silently compiled the
    // esp_read_mac() path out, which is what produced the reversed name above.
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        fromEfuse = true;
        const uint64_t efuse = ESP.getEfuseMac();
        for (int i = 0; i < 6; i++) {
            mac[i] = (uint8_t)((efuse >> (8 * i)) & 0xFF);   // low byte first
        }
    }

    char hex[13];
    snprintf(hex, sizeof(hex), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    const int len = (int)strlen(hex);
    const char *suffix = (len >= BLE_NAME_SUFFIX_LEN) ? hex + (len - BLE_NAME_SUFFIX_LEN) : hex;

    snprintf(s_deviceName, sizeof(s_deviceName), "%s%s", BLE_NAME_PREFIX, suffix);
    BLE_LOG("[MAIN] device name %s (mac %s%s)\n", s_deviceName, hex,
            fromEfuse ? ", read from eFuse" : "");
}

// The panel pins are configured on first use, never at boot: configuring RST as
// an output is harmless in principle, but "power on must not disturb the panel"
// is easier to guarantee by simply not touching any of it until we are about to
// paint.
static void ensurePanelGpio() {
    if (s_gpioReady) return;
    epd398GpioInit();
    s_gpioReady = true;
}

static bool paintHint(const char *why, uint8_t opcode) {
    if (!s_frame) return false;

    // Claim the framebuffer for the whole 15-25 s paint. Without this a phone
    // could BEGIN_IMAGE and stream a picture into the very bytes being pushed
    // to the panel (and BEGIN_IMAGE would not even answer ERR_BUSY).
    if (!bleBeginHintPaint()) {
        BLE_LOG("[MAIN] hint not painted (%s): panel buffer busy\n", why);
        return false;
    }

    ensurePanelGpio();
    BLE_LOG("[MAIN] painting hint screen (%s)\n", why);
    ledSetMode(LedMode::SlowBlink);
    bleNotifyRendering();          // explains the ERR_BUSY a fast app may see
    const bool ok = bleScreenShowHint(s_frame, s_deviceName);

    // Record the refresh while the buffer is still claimed (same order as the
    // image path), then release it. A BEGIN_IMAGE arriving in between must not
    // be accepted against a stale cooldown.
    refreshGuardNoteRefresh(opcode);
    bleRenderFinished();

    if (ok) {
        s_hintShown = true;        // the automatic hint already ran
        s_errorHold = false;
    } else {
        // Do NOT mark the hint as shown: the idle timer may retry once the
        // cooldown has passed again.
        s_errorHold = true;
        s_errorSince = millis();
        ledSetMode(LedMode::FastBlink);
    }

    bleNotifyState(ok ? BLE_STATE_HINT_SHOWN : BLE_STATE_ERROR,
                   ok ? BLE_OK : BLE_ERR_PANEL);
    return true;
}

// Drop a pending hint request and its bookkeeping. Every path that finishes,
// replaces or abandons a deferred hint needs the same four fields cleared, and
// keeping it in one place is what stops them from drifting apart.
static void clearHintRequest() {
    s_hintWanted = false;
    s_hintWhy = nullptr;
    s_hintOpcode = 0;
    s_hintAuto = false;
}

// Ask for the hint screen, respecting the panel's cool-down.
//
// The automatic paths (idle deadline, BOOT button) have no app to ask, so a
// hint that arrives too early simply waits. A hint the app asked for has
// already been through the confirmation handshake, so only a grant issued for
// SHOW_HINT can admit it (opcode binding).
//
// `automatic` marks the one request whose premise can expire while it waits -
// the idle deadline (nobody connected). The BOOT button passes 0 as its opcode
// for the same reason as the idle path (nothing was refused, so no grant can
// apply to it) but it is a deliberate user action, so it is not automatic.
static void requestHint(const char *why, uint8_t opcode, bool automatic) {
    if (!s_frame) return;

    // Admission is always the guard's job: an automatic hint (opcode 0) must
    // wait for the cooldown, and an app hint may only use a grant issued for
    // SHOW_HINT. Never call paintHint() before this check.
    if (refreshGuardMayRefreshFor(opcode) && paintHint(why, opcode)) {
        clearHintRequest();
        return;
    }

    // A request that is already waiting is not replaced by a later one. The
    // idle deadline has NOT restarted, so overwriting its why/opcode would let a
    // fresh request fire the moment the cool-down ends - the one thing the panel
    // must not get. Keeping the original entry also keeps the log honest.
    if (s_hintWanted) {
        BLE_LOG("[MAIN] hint already deferred (%s), keeping it over (%s)\n",
                s_hintWhy ? s_hintWhy : "?", why);
        return;
    }

    s_hintWanted = true;
    s_hintWhy = why;
    s_hintOpcode = opcode;
    s_hintAuto = automatic;
    if (refreshGuardMayRefreshFor(opcode)) {
        BLE_LOG("[MAIN] hint deferred (%s): panel buffer busy\n", why);
    } else {
        BLE_LOG("[MAIN] hint deferred (%s): cool-down, %lu s left\n", why,
                (unsigned long)refreshGuardWaitSeconds());
    }
}

static void paintImage(uint8_t format, uint32_t bytes) {
    if (!s_frame) {
        // Defensive: bleTakeRenderRequest() should have refused to claim the
        // buffer without one.
        bleRenderFinished();
        return;
    }

    // A picture always wins over a hint that was waiting for the cool-down:
    // painting the hint afterwards would erase the picture the user just sent.
    clearHintRequest();

    // The buffer already holds the picture in panel-native layout: ble_service
    // rotated it out of the phone's portrait canvas while receiving. There is
    // nothing to convert here, only to push.
    BLE_LOG("[MAIN] painting image: format %u, %lu canvas bytes\n",
            (unsigned)format, (unsigned long)bytes);
    ensurePanelGpio();

    ledSetMode(LedMode::SlowBlink);
    bleNotifyRendering();

    epd398Display2bpp(s_frame);
    const bool ok = epd398LastOperationOk();

    // Start the cool-down, and consume the confirmation grant if the app went
    // through the warning dialog for this image. Done before the app is told
    // DISPLAYED, so a refresh that starts the instant it hears the news is
    // already inside the new cool-down.
    refreshGuardNoteRefresh(BLE_OP_BEGIN_IMAGE);

    // Hand the framebuffer back before announcing the result, so an app that
    // starts the next image the instant it sees DISPLAYED is not answered with
    // ERR_BUSY for no reason.
    bleRenderFinished();

    // Only a picture that actually reached the glass suppresses the idle hint
    // and counts as "an image is up". A failed refresh leaves the panel
    // unchanged, so the hint remains available.
    if (ok) {
        s_imageShown = true;
        s_hintShown = true;
        s_errorHold = false;
    }

    bleNotifyDisplayed(ok ? BLE_OK : BLE_ERR_PANEL);
    if (ok) {
        BLE_LOG("[MAIN] picture displayed\n");
        ledFeedback("ack");
    } else {
        BLE_LOG("[MAIN] panel refresh failed (BUSY timeout)\n");
        s_errorHold = true;
        s_errorSince = millis();
        ledSetMode(LedMode::FastBlink);
    }
}

// GPIO9 short press -> repaint the hint; hold -> reboot.
static void checkButton() {
    const bool down = (digitalRead(PIN_CFG_BTN) == LOW);
    const unsigned long now = millis();

    if (down && !s_btnDown) {
        s_btnDown = true;
        s_btnDownAt = now;
        s_btnFired = false;
    } else if (down && s_btnDown && !s_btnFired) {
        if (now - s_btnDownAt >= CFG_BTN_REBOOT_MS) {
            s_btnFired = true;      // fire once, however long the button is held
            BLE_LOG("[BTN] held %lums -> restart\n", now - s_btnDownAt);
            ledFeedback("off");
            delay(50);
            ESP.restart();
        }
    } else if (!down && s_btnDown) {
        const unsigned long held = now - s_btnDownAt;
        s_btnDown = false;
        if (!s_btnFired && held >= BTN_DEBOUNCE_MS) {
            BLE_LOG("[BTN] short press (%lums) -> hint screen\n", held);
            requestHint("button", 0, false);
        }
    }
}

static bool errorHoldActive() {
    if (!s_errorHold) return false;
    if ((unsigned long)(millis() - s_errorSince) < 10000UL) return true;
    s_errorHold = false;   // rollover-safe: subtraction, not millis() < until
    return false;
}

static void updateLed() {
    if (errorHoldActive()) return;   // a failure owns the LED for a while
    if (bleIsTransferring()) {
        ledSetMode(LedMode::FastBlink);
    } else if (bleIsConnected()) {
        ledSetMode(LedMode::Solid);
    } else {
        ledSetMode(LedMode::SlowBlink);
    }
}

// ---------------------------------------------------------------- setup / loop

void setup() {
    ledInit();

    Serial.begin(115200);
    delay(1500);                 // let the USB CDC console come up
    Serial.println("\n=== 8adge BLE (no WiFi) ===");
    Serial.printf("[MAIN] protocol v%u, firmware %u.%u.%u\n",
                  (unsigned)BLE_PROTOCOL_VERSION,
                  (unsigned)FW_VERSION_MAJOR, (unsigned)FW_VERSION_MINOR,
                  (unsigned)FW_VERSION_PATCH);

    pinMode(PIN_CFG_BTN, INPUT_PULLUP);

    // Reset the refresh cool-down for this power cycle. Called before anything
    // can paint, and it is what makes the first unsafe refresh of a boot
    // impossible: with no clock yet, the guard has to assume the last refresh
    // was the instant before the lights went out.
    refreshGuardInit();

    // Allocate before the BLE stack claims its share of the heap. This is the
    // largest single allocation in the firmware and it must be contiguous.
    s_frame = epd398AllocBuffer();
    if (!s_frame) {
        Serial.println("[MAIN] FATAL: 105984-byte framebuffer allocation failed");
    }

    buildDeviceName();
    if (!bleInit(s_deviceName, s_frame)) {
        Serial.println("[MAIN] WARNING: no framebuffer, image transfers will be refused");
    }

    s_idleSince = millis();
    s_wasConnected = false;
    ledFeedback("waiting");

    Serial.printf("[MAIN] hint screen in %lu s if nothing connects\n",
                  (unsigned long)(BLE_HINT_DELAY_MS / 1000UL));
    Serial.println("[MAIN] panel left untouched on purpose: e-ink keeps the last picture");
}

void loop() {
    ledTick();
    blePollTransferTimeout();

    // Link transitions: a disconnect restarts the idle countdown, so a phone
    // that connected and went away still gets the hint two minutes later.
    const bool connected = bleIsConnected();
    if (s_wasConnected && !connected) {
        s_idleSince = millis();
        BLE_LOG("[MAIN] link idle, hint countdown restarted\n");
    }
    s_wasConnected = connected;

    uint8_t format = 0;
    uint32_t bytes = 0;
    // A paint holds the loop for 15-25 s, so a hint request that arrived during
    // it waits for the next iteration rather than being painted straight after -
    // two full refreshes back to back would be 50 s of unusable device.
    if (bleTakeRenderRequest(&format, &bytes)) {
        paintImage(format, bytes);
    } else if (bleTakeHintRequest()) {
        requestHint("app request", BLE_OP_SHOW_HINT, false);
    }

    // A hint that had to wait for the cool-down (or for the buffer) goes out the
    // moment it is safe. The opcode is preserved so an app confirmation for
    // SHOW_HINT is not lost, and vice versa.
    //
    // An AUTOMATIC (idle-deadline) request is dropped once its premise is gone:
    // a phone connected after the idle deadline means nobody is waiting for
    // instructions any more, and the app may have opted out of automatic hints
    // meanwhile. Painting it then would burn the cool-down right before the
    // upload the user came to do. A BOOT press and an app request are explicit
    // (s_hintAuto is false), so those keep waiting.
    if (s_hintWanted && s_hintAuto &&
        (connected || (bleOptions() & BLE_OPT_DISABLE_HINT))) {
        BLE_LOG("[MAIN] deferred hint (%s) dropped: %s\n",
                s_hintWhy ? s_hintWhy : "?",
                connected ? "a phone connected" : "automatic hint disabled by the app");
        clearHintRequest();
    }

    if (s_hintWanted && s_frame && !bleIsTransferring() &&
        refreshGuardMayRefreshFor(s_hintOpcode)) {
        const char *why = s_hintWhy ? s_hintWhy : "deferred";
        const uint8_t op = s_hintOpcode;
        const bool automatic = s_hintAuto;
        clearHintRequest();
        if (paintHint(why, op)) {
            // Painted; nothing stays pending.
        } else {
            // Lost the race for the framebuffer to an image transfer, so the
            // request has to survive it, origin included.
            s_hintWanted = true;
            s_hintWhy = why;
            s_hintOpcode = op;
            s_hintAuto = automatic;
        }
    }

    checkButton();

    // The 2-minute rule. Suppressed once a picture is up (never overwrite the
    // user's image), while a request is already pending (otherwise the log is
    // written every 5 ms for the whole cooldown), and when the app has opted
    // out with SET_OPTIONS.
    if (!s_hintShown && !s_imageShown && !s_hintWanted && !connected &&
        !(bleOptions() & BLE_OPT_DISABLE_HINT) &&
        (millis() - s_idleSince) >= BLE_HINT_DELAY_MS) {
        requestHint("idle 2 min", 0, true);
    }

    updateLed();
    delay(5);
}
