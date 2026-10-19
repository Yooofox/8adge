#ifndef BADGE8_REFRESH_GUARD_H
#define BADGE8_REFRESH_GUARD_H

#include <Arduino.h>
#include <stdint.h>

// ============================================================================
//  Full-refresh cooldown - panel protection against irreversible ghosting.
//
//  A 4-colour e-paper refresh is not "set a pixel": the controller drives a
//  long bipolar AC waveform (15-25 s here) that physically moves red, yellow,
//  black and white pigment particles through the microcapsule fluid. The
//  coloured pigments have low electrophoretic mobility, so a refresh that
//  starts before the previous one has settled makes particles of opposite
//  polarity clump together. The result is irreversible: colour contamination,
//  a red-tinted white, and grey ghosting that no amount of "refresh to white"
//  removes. Charge also accumulates on the ITO layer and the dielectric film.
//
//  Panel vendors (E Ink, Good Display, Waveshare) therefore specify a minimum
//  interval of >= 180 s between full refreshes on 4-colour parts, and most
//  disable partial refresh entirely in the controller. This firmware has no
//  partial-refresh path, but nothing stopped an app - or a user hammering the
//  BOOT button - from repainting every few seconds. This module is that brake.
//
//  ------------------------------------------------------------------ the clock problem
//
//  The cooldown needs "how long ago was the last refresh", and this board has
//  no way to know the time on its own:
//
//    * the device never connects to anything (BLE only, no WiFi, no SNTP);
//    * millis() restarts at 0 on every power-up;
//    * RTC slow memory (RTC_DATA_ATTR) survives deep sleep and a software
//      reset, but NOT a power cut - and the C3-SuperMini has no VBAT pin, so
//      the RTC domain dies with the supply. Its contents are also undefined
//      after a power-on reset;
//    * NVS keeps a number, but a number is useless without knowing "now".
//
//  So the *clock comes from the app*: it sends BLE_OP_SYNC_TIME with UTC
//  seconds right after connecting (see docs/BLE_PROTOCOL.md). The device then
//  keeps time itself by anchoring that value to its own monotonic uptime, and
//  persists the last refresh to NVS so a long power-off is not punished.
//
//  ------------------------------------------------------------------ the invariant
//
//  Guaranteeing the cooldown cannot depend on having a clock, because the idle
//  hint screen has to be drawn when nobody is connected - i.e. exactly when no
//  clock is available. That is what this inequality is for:
//
//      real elapsed time since the last refresh  >=  uptime
//
//  because the last refresh happened either before this power-up (so the real
//  elapsed time is the off-air time *plus* uptime, and off-air time >= 0) or
//  during this power-up (so the uptime difference is the elapsed time exactly).
//
//  "Wait until uptime >= 180 s" is therefore always compliant, only ever
//  conservative. With a clock the rule becomes exact; with a stored NVS
//  timestamp it is also *permissive*, because a device that was unplugged for
//  three days does not have to sit through 180 s of uptime before its first
//  refresh.
//
//  ------------------------------------------------------------------ two states of the record
//
//  A refresh that happens while the clock is unknown cannot be dated. Such a
//  refresh is recorded as UNDATED and the guard then keeps using the uptime
//  bound for the rest of that power cycle - including after the app finally
//  syncs, because using the older dated timestamp would overestimate how long
//  ago the newest refresh was, which is the one unsafe direction. UNDATED is
//  persisted, so a power cut between the undated refresh and the sync cannot
//  resurrect a stale timestamp either.
// ============================================================================

// Load the persisted record and reset the session's uptime reference.
// Call once from setup(), before the panel can be painted.
void refreshGuardInit();

// ---------------------------------------------------------------- clock

// Adopt the app's clock: `unixUtc` is seconds since the epoch, UTC.
//
// UTC and not local time on purpose: only differences are ever computed, and a
// difference is a timezone-independent quantity - but a *local* timestamp is
// not, because a DST transition inside the interval would shift the difference
// by an hour. Sending UTC removes the whole class of problems.
//
// Returns false for an implausible value (before 2001 or after 2100), which is
// what catches the classic mistake of sending milliseconds instead of seconds.
bool refreshGuardSyncClock(uint64_t unixUtc);

// True once the app has supplied a clock this power cycle.
bool refreshGuardClockKnown();

// Current UTC seconds, or 0 when the clock is unknown.
uint32_t refreshGuardNowUtc();

// The UTC timestamp of the last refresh, or 0 when there is none or it could
// not be dated.
uint32_t refreshGuardLastRefreshUtc();

// ---------------------------------------------------------------- the rule

// The advertised minimum interval between full refreshes, in seconds.
uint16_t refreshGuardMinGapSeconds();

// True when the cooldown has elapsed. This is the rule alone; it knows nothing
// about a user confirmation (see refreshGuardMayRefresh()).
bool refreshGuardAllowed();

// True when a full refresh may be started right now for `opcode`
// (BLE_OP_BEGIN_IMAGE / BLE_OP_SHOW_HINT): the cooldown has elapsed, or the app
// obtained a confirmation grant *for that same command* after showing the user
// a warning. A grant is never shared between BEGIN_IMAGE and SHOW_HINT, and the
// idle/BOOT hint (opcode 0) can never consume an app's grant.
//
// The clock is untrusted. Within a power cycle the adopted time may not run
// ahead of the device's own uptime by more than BLE_CLOCK_MAX_FORWARD_SKEW_S,
// so a hostile app cannot fast-forward the cooldown by re-syncing. The first
// sync after power-up cannot be validated (no trusted time source exists on
// this board); see the "trust boundary" note in docs/BLE_PROTOCOL.md.
bool refreshGuardMayRefreshFor(uint8_t opcode);

// Backwards-compatible shorthand for "no app command": cooldown only, never a
// confirmation grant. Used by the autonomous hint paths.
bool refreshGuardMayRefresh();

// Seconds still to wait; 0 when refreshGuardAllowed() is true.
uint32_t refreshGuardWaitSeconds();

// Best estimate of how long ago the last refresh was. With a clock this is
// exact; without one it is the lower bound described in the invariant above,
// so it is safe to show to a user as "at least this long ago".
uint32_t refreshGuardElapsedSeconds();

// Reason code for EVT_REFRESH_BLOCKED (BLE_GUARD_REASON_*, ble_protocol.h).
uint8_t refreshGuardReason();

// ---------------------------------------------------------------- confirmation

// Mint the token that identifies one blocked request, together with the command
// that was refused. The app echoes the token back in BLE_OP_CONFIRM_REFRESH; a
// fresh call invalidates the previous token.
uint8_t refreshGuardIssueToken(uint8_t forOpcode);

// Arm a confirmation grant if `token` is the live token and has not expired.
// Returns false for a stale, unknown or already-confirmed token, and also when
// the previous refresh was itself forced and has not yet aged past the full
// cooldown (a second forced refresh could otherwise damage the panel twice in
// a row). The grant is bound to the opcode that was refused.
bool refreshGuardArmGrant(uint8_t token);

// True while a confirmation grant is armed and unexpired. The grant survives
// until the panel is actually painted, so a transfer that fails on CRC does not
// force the user to confirm a second time.
bool refreshGuardGrantLive();

// Record that the panel has just completed a full refresh: moves the cooldown
// reference, dates it if the clock is known, persists it, and consumes a
// confirmation grant if (and only if) it belongs to `opcode` - the image path
// passes BLE_OP_BEGIN_IMAGE, an app-requested hint BLE_OP_SHOW_HINT, and the
// autonomous hint paths 0, so an unrelated approval is never cancelled.
// MUST be called after every full refresh - the image path, the hint screen,
// and a test pattern alike - or the guarantee is void.
void refreshGuardNoteRefresh(uint8_t opcode);

#endif // BADGE8_REFRESH_GUARD_H