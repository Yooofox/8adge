// Onboard status LED: a small, non-blocking state machine.
//
// The device is a "headless" gadget whose only feedback channel is one blue LED
// (GPIO8 on the C3-SuperMini, active low - see PIN_LED in config.h), so the LED
// has to carry four different meanings at once:
//
//   Solid      "everything is fine" - a phone is connected, or a picture is up
//   SlowBlink  "working on it"       - advertising, or refreshing the panel
//   FastBlink  "needs attention"     - transfer running, or an error
//   Off        "asleep"
//
// Two entry points keep call sites simple:
//
//   ledSetMode(mode)     set the steady state
//   ledFeedback(pattern) one-shot acknowledgment; "waiting" and "off" also move
//                        the steady state, so a call site never has to remember
//                        to do both. See the declaration for the patterns.
//
// Nothing here blocks: ledTick() advances a blink from millis(), and ledDelay()
// is a delay() replacement that keeps ticking. ledDelay() is what lets the LED
// keep blinking while the panel is busy for 25 s inside a refresh.
#pragma once

#include <stdint.h>

enum class LedMode : uint8_t {
    Off,        // steady off - sleeping
    Solid,      // steady on  - ready
    FastBlink,  // ~5 Hz      - problem / waiting for input
    SlowBlink,  // ~0.8 Hz    - busy
};

/** Configure the pin and park the LED off. Safe to call before anything else. */
void ledInit();

/** Set the steady state. No-op when the mode is already active. */
void ledSetMode(LedMode mode);

/** Current steady state. */
LedMode ledMode();

/** Advance a blink. Cheap; call it from every loop iteration. */
void ledTick();

/** delay() that keeps ledTick()ing, so blinks survive long blocking waits. */
void ledDelay(unsigned long ms);

/**
 * One-shot pattern: "ack" (two short blinks, leaves the steady state alone),
 * "waiting" (start slow-blinking) or "off". Unknown patterns are ignored and
 * leave the LED untouched, so a stale call site cannot park it in a state
 * nothing ever changes back.
 */
void ledFeedback(const char *pattern);
