#include "led.h"

#include <Arduino.h>
#include <string.h>

#include "config.h"

// Onboard blue LED, ACTIVE LOW (see config.h). Every write goes through these
// two macros so the polarity lives in exactly one place, and both compile to
// nothing when PIN_LED < 0 - ledFeedback() writes the pin directly, so gating
// only ledApplyPin() would have left a config knob that does not do what its
// comment says.
#if PIN_LED >= 0
#define LED_ON()   digitalWrite(PIN_LED, PIN_LED_ACTIVE_LOW ? LOW : HIGH)
#define LED_OFF()  digitalWrite(PIN_LED, PIN_LED_ACTIVE_LOW ? HIGH : LOW)
#else
#define LED_ON()   do {} while (0)
#define LED_OFF()  do {} while (0)
#endif

namespace {

// Half periods. Fast is a "something is wrong" stutter, slow is a calm
// "working" pulse that stays readable while the panel refreshes for 25 s.
constexpr unsigned long FAST_HALF_MS = 100;   // ~5 Hz
constexpr unsigned long SLOW_HALF_MS = 600;   // ~0.8 Hz

LedMode g_mode = LedMode::Off;
bool g_lit = false;
unsigned long g_lastToggleAt = 0;

void ledApplyPin() {
    if (g_lit) {
        LED_ON();
    } else {
        LED_OFF();
    }
}

unsigned long ledHalfPeriod(LedMode mode) {
    switch (mode) {
        case LedMode::FastBlink: return FAST_HALF_MS;
        case LedMode::SlowBlink: return SLOW_HALF_MS;
        default:                 return 0;
    }
}

// Move the steady state and put the pin in a deterministic phase: Solid/Off
// settle immediately, a blink starts lit so the transition is visible at once.
void ledApplyMode(LedMode mode, bool restartPhase) {
    g_mode = mode;
    g_lit = (mode != LedMode::Off);
    if (restartPhase) {
        g_lastToggleAt = millis();
    }
    ledApplyPin();
}

}  // namespace

void ledInit() {
#if PIN_LED >= 0
    pinMode(PIN_LED, OUTPUT);
#endif
    g_mode = LedMode::Off;
    g_lit = false;
    LED_OFF();   // GPIO8 is a strapping pin: park it OFF, never ON
}

void ledSetMode(LedMode mode) {
    if (mode == g_mode) return;
    ledApplyMode(mode, true);
}

LedMode ledMode() {
    return g_mode;
}

void ledTick() {
    unsigned long half = ledHalfPeriod(g_mode);
    if (half == 0) return;   // Off / Solid need no work
    unsigned long now = millis();
    if (now - g_lastToggleAt < half) return;
    g_lastToggleAt = now;
    g_lit = !g_lit;
    ledApplyPin();
}

void ledDelay(unsigned long ms) {
    unsigned long startedAt = millis();
    while (millis() - startedAt < ms) {
        ledTick();
        // The panel refresh blocks the loop for 15-25 s through this helper.
        // delay() yields to FreeRTOS, which is what keeps the BLE host task and
        // its connection alive while the screen is being painted.
        delay(5);
    }
    ledTick();
}

void ledFeedback(const char *pattern) {
    if (!pattern) return;

    LedMode next = g_mode;

    if (strcmp(pattern, "ack") == 0) {
        for (int i = 0; i < 2; i++) {
            LED_ON();  delay(80);
            LED_OFF(); delay(80);
        }
    } else if (strcmp(pattern, "waiting") == 0) {
        // Advertising and no phone connected yet: a calm, unmissable pulse.
        next = LedMode::SlowBlink;
    } else if (strcmp(pattern, "off") == 0) {
        next = LedMode::Off;
    } else {
        // Unknown pattern: leave the LED alone. The three above are the only
        // ones this firmware uses; the "connecting"/"success"/"fail" family the
        // header used to list came from the WiFi build and had no call site
        // left, so it was removed rather than kept as documentation that
        // describes nothing.
        return;
    }

    // Re-assert the steady state, restarting the blink phase so the pin never
    // stays at whatever the last blink left behind.
    ledApplyMode(next, true);
}
