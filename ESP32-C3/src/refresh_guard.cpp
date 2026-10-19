// ============================================================================
//  Full-refresh cooldown - see refresh_guard.h for why this exists and for the
//  reasoning behind the uptime invariant and the two-state NVS record.
//
//  Storage: one 16-byte record in NVS (namespace "8adge", key "rec"). Flash is
//  written once per *refresh*, and a refresh is rate-limited to one per 180 s by
//  this very module, so the worst case is 480 writes a day - a NVS page holds
//  well over a hundred entries, so that is a handful of page erases a day
//  against an endurance of ~100k. Decades, not years.
//
//  The record is a struct rather than several keys on purpose: one key means
//  one NVS entry, so a refresh costs a single write and the fields can never
//  end up mutually inconsistent (a timestamp from one refresh next to a state
//  byte from another).
// ============================================================================

#include "refresh_guard.h"

#include <Preferences.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "ble_protocol.h"
#include "config.h"

// The wire constant lives in ble_protocol.h (the app reads it) and the
// millisecond form in config.h (the firmware uses it). They must agree.
static_assert(BLE_REFRESH_MIN_GAP_MS == (unsigned long)BLE_REFRESH_MIN_GAP_S * 1000UL,
              "config.h BLE_REFRESH_MIN_GAP_MS disagrees with ble_protocol.h "
              "BLE_REFRESH_MIN_GAP_S");

namespace {

constexpr char NVS_NAMESPACE[] = "8adge";   // NVS names are capped at 15 chars
constexpr char NVS_KEY[] = "rec";
constexpr uint32_t RECORD_MAGIC = 0x38424744UL;   // "DGB8" little-endian
constexpr uint8_t RECORD_VERSION = 2;
constexpr uint8_t RECORD_VERSION_LEGACY = 1;

// What the record means. An *absent* record is the absence of a record; a
// *corrupt* one is treated as UNDATED so it can never be read as "nothing was
// ever refreshed", which would allow an immediate refresh.
constexpr uint8_t REC_UNDATED = 1;   // a refresh happened, we cannot say when
constexpr uint8_t REC_DATED = 2;     // ... and lastRefreshUtc is its UTC time

// flags bit 0: the refresh that produced this record was forced by a user
// confirmation (i.e. it happened inside the cooldown).
constexpr uint8_t REC_FLAG_FORCED = 0x01;

// 16 bytes. The first 12 bytes are layout-compatible with the v1 record, so an
// upgrade keeps the last-refresh timestamp instead of starting blind.
struct GuardRecord {
    uint32_t magic;
    uint8_t  version;
    uint8_t  state;
    uint8_t  flags;
    uint8_t  reserved;
    uint32_t lastRefreshUtc;   // valid only when state == REC_DATED
    uint32_t reserved2;
};

Preferences g_prefs;
bool g_prefsReady = false;

// Both the NimBLE host task and the Arduino loop task call into this module
// (SYNC_TIME/CONFIRM on one, the paint paths on the other). A recursive mutex
// keeps every multi-field update consistent, including nested public calls
// such as refreshGuardMayRefreshFor() -> allowedUnlocked().
SemaphoreHandle_t g_mux = nullptr;

struct GuardLock {
    GuardLock() { if (g_mux) xSemaphoreTakeRecursive(g_mux, portMAX_DELAY); }
    ~GuardLock() { if (g_mux) xSemaphoreGiveRecursive(g_mux); }
    GuardLock(const GuardLock &) = delete;
    GuardLock &operator=(const GuardLock &) = delete;
};

// What we know about the last refresh:
//   g_haveRecord      - any refresh is known to have happened
//   g_hasWall         - ... and it carries an exact UTC timestamp
//   g_lastRefreshUtc  - that timestamp (only meaningful with g_hasWall)
//   g_refUptimeMs     - uptime at which it happened. 0 means "before this
//                       boot", which is also the initial value, so the uptime
//                       lower bound is correct from the first millisecond.
bool     g_haveRecord = false;
bool     g_hasWall = false;
bool     g_lastRefreshForced = false;
uint32_t g_lastRefreshUtc = 0;
uint32_t g_refUptimeMs = 0;

// The app-supplied clock, anchored to our own monotonic uptime. g_syncStart*
// is the *first* accepted sync of this power cycle and is what bounds
// cumulative forward jumps.
bool     g_clockKnown = false;
uint32_t g_clockUtc = 0;
uint32_t g_clockUptimeMs = 0;
uint32_t g_syncStartUtc = 0;
uint32_t g_syncStartMs = 0;

// Confirmation handshake. A grant is bound to the command that was refused.
uint8_t  g_pendingToken = 0;
uint32_t g_pendingAt = 0;
uint8_t  g_pendingOpcode = 0;
uint8_t  g_grantToken = 0;
uint32_t g_grantAt = 0;
uint8_t  g_grantOpcode = 0;

// Plausibility window for an app-supplied timestamp: 2001-09-09 .. 2100-01-01.
// Deliberately generous - this only has to reject nonsense (0, a millisecond
// value, a date in the 1970s), not police clock accuracy.
constexpr uint64_t CLOCK_MIN_UTC = 1000000000ULL;
constexpr uint64_t CLOCK_MAX_UTC = 4102444800ULL;

bool elapsed(uint32_t since, uint32_t limitMs) {
    // Unsigned subtraction on purpose: correct across the millis() rollover at
    // ~49.7 days, which this device can easily reach because it never reboots.
    return (uint32_t)(millis() - since) >= limitMs;
}

uint32_t nowUtcUnlocked() {
    if (!g_clockKnown) return 0;
    return g_clockUtc + (uint32_t)((millis() - g_clockUptimeMs) / 1000UL);
}

// A wall-clock timestamp is only usable while the app's clock is at or after
// it. A clock behind the record means the record (or the clock) is wrong;
// falling back to the uptime bound keeps the panel safe for at least one full
// cooldown and lets the device recover instead of blocking forever.
bool wallUsableUnlocked() {
    return g_clockKnown && g_hasWall && nowUtcUnlocked() >= g_lastRefreshUtc;
}

bool allowedUnlocked() {
    // Nothing to protect against: a device that has never painted a full frame
    // since it was built has no waveform to recover from.
    if (!g_haveRecord) return true;

    if (wallUsableUnlocked()) {
        return (nowUtcUnlocked() - g_lastRefreshUtc) >=
               (uint32_t)BLE_REFRESH_MIN_GAP_S;
    }

    // No trustworthy clock: the uptime lower bound from the header.
    return elapsed(g_refUptimeMs, BLE_REFRESH_MIN_GAP_MS);
}

void persist(uint8_t state, uint32_t utc, bool forced) {
    if (!g_prefsReady) return;

    GuardRecord rec;
    rec.magic = RECORD_MAGIC;
    rec.version = RECORD_VERSION;
    rec.state = state;
    rec.flags = forced ? REC_FLAG_FORCED : 0;
    rec.reserved = 0;
    rec.lastRefreshUtc = utc;
    rec.reserved2 = 0;

    if (g_prefs.putBytes(NVS_KEY, &rec, sizeof(rec)) != sizeof(rec)) {
        BLE_LOG("[GUARD] NVS write failed, the cooldown will not survive a power cut\n");
    }
}

}  // namespace

// ---------------------------------------------------------------- init

void refreshGuardInit() {
    if (!g_mux) g_mux = xSemaphoreCreateRecursiveMutex();

    GuardLock lock;

    // No refresh yet this session. 0 is not a placeholder: it makes the uptime
    // lower bound read as "millis() since boot", which is exactly the bound
    // required when the newest refresh predates this boot.
    g_refUptimeMs = 0;
    g_haveRecord = false;
    g_hasWall = false;
    g_lastRefreshForced = false;
    g_lastRefreshUtc = 0;
    g_clockKnown = false;
    g_syncStartUtc = 0;
    g_syncStartMs = 0;
    g_pendingToken = 0;
    g_grantToken = 0;

    g_prefsReady = g_prefs.begin(NVS_NAMESPACE, /*readOnly=*/false);
    if (!g_prefsReady) {
        // Without NVS the guard still works, it just cannot tell a three-day
        // power-off from a five-second one: every cold boot then has to serve
        // the full cooldown as uptime. Safe, only less convenient.
        BLE_LOG("[GUARD] NVS unavailable: cooldown is uptime-only this session\n");
        return;
    }

    GuardRecord rec;
    memset(&rec, 0, sizeof(rec));
    const size_t got = g_prefs.getBytes(NVS_KEY, &rec, sizeof(rec));

    if (got == 0) {
        BLE_LOG("[GUARD] no refresh record (fresh device?)\n");
        return;
    }

    const bool legacy = (got == 12 && rec.version == RECORD_VERSION_LEGACY);
    const bool current = (got == sizeof(rec) && rec.version == RECORD_VERSION);
    if ((!legacy && !current) || rec.magic != RECORD_MAGIC) {
        // A corrupt record must not be read as "nothing was ever refreshed".
        g_haveRecord = true;
        g_hasWall = false;
        BLE_LOG("[GUARD] refresh record unreadable -> uptime-only cooldown\n");
        return;
    }

    g_lastRefreshForced = (rec.flags & REC_FLAG_FORCED) != 0;

    if (rec.state == REC_DATED && rec.lastRefreshUtc != 0) {
        g_haveRecord = true;
        g_hasWall = true;
        g_lastRefreshUtc = rec.lastRefreshUtc;
        BLE_LOG("[GUARD] last refresh %lu (utc%s), waiting for the app's clock\n",
                (unsigned long)g_lastRefreshUtc,
                g_lastRefreshForced ? ", forced" : "");
    } else if (rec.state == REC_UNDATED) {
        // A refresh we could not date happened before this boot. From here on
        // the uptime bound is the only honest answer, and it will not be
        // replaced by a wall-clock rule even once the app syncs - the stored
        // timestamp is older than that refresh and would understate the wait.
        g_haveRecord = true;
        g_hasWall = false;
        BLE_LOG("[GUARD] last refresh is undated, cooldown is uptime-only\n");
    } else {
        // Unknown state, or a dated record without a timestamp: treat it as a
        // refresh we cannot date (the safe direction).
        g_haveRecord = true;
        g_hasWall = false;
        BLE_LOG("[GUARD] refresh record has no usable timestamp -> uptime-only\n");
    }
}

// ---------------------------------------------------------------- clock

bool refreshGuardSyncClock(uint64_t unixUtc) {
    if (unixUtc < CLOCK_MIN_UTC || unixUtc > CLOCK_MAX_UTC) {
        BLE_LOG("[GUARD] rejected clock %llu (utc seconds expected)\n",
                (unsigned long long)unixUtc);
        return false;
    }

    GuardLock lock;
    const uint32_t candidate = (uint32_t)unixUtc;

    if (!g_clockKnown) {
        // First accepted sync of this power cycle: it becomes the anchor that
        // bounds every later sync. This first value is a trust boundary - the
        // board has no other time source - but after it, the clock can never
        // run ahead of real uptime again.
        g_syncStartUtc = candidate;
        g_syncStartMs = millis();
    } else {
        const uint32_t uptimeS = (uint32_t)((millis() - g_syncStartMs) / 1000UL);
        const uint32_t limit = g_syncStartUtc + uptimeS +
                               (uint32_t)BLE_CLOCK_MAX_FORWARD_SKEW_S;
        if (candidate > limit) {
            BLE_LOG("[GUARD] rejected clock %lu: more than %u s ahead of the "
                    "uptime bound (%lu)\n",
                    (unsigned long)candidate,
                    (unsigned)BLE_CLOCK_MAX_FORWARD_SKEW_S,
                    (unsigned long)limit);
            return false;
        }
    }

    g_clockUtc = candidate;
    g_clockUptimeMs = millis();
    g_clockKnown = true;
    BLE_LOG("[GUARD] clock set to %lu (utc)\n", (unsigned long)g_clockUtc);
    return true;
}

bool refreshGuardClockKnown() {
    GuardLock lock;
    return g_clockKnown;
}

uint32_t refreshGuardNowUtc() {
    GuardLock lock;
    return nowUtcUnlocked();
}

uint32_t refreshGuardLastRefreshUtc() {
    GuardLock lock;
    return g_hasWall ? g_lastRefreshUtc : 0;
}

// ---------------------------------------------------------------- the rule

uint16_t refreshGuardMinGapSeconds() {
    return (uint16_t)BLE_REFRESH_MIN_GAP_S;
}

bool refreshGuardAllowed() {
    GuardLock lock;
    return allowedUnlocked();
}

bool refreshGuardGrantLive() {
    GuardLock lock;
    if (g_grantToken == 0) return false;
    if (elapsed(g_grantAt, BLE_REFRESH_GRANT_MS)) {
        g_grantToken = 0;
        BLE_LOG("[GUARD] confirmation grant expired unused\n");
        return false;
    }
    return true;
}

bool refreshGuardMayRefreshFor(uint8_t opcode) {
    GuardLock lock;

    if (allowedUnlocked()) return true;

    if (g_grantToken == 0) return false;
    if (elapsed(g_grantAt, BLE_REFRESH_GRANT_MS)) {
        g_grantToken = 0;
        BLE_LOG("[GUARD] confirmation grant expired unused\n");
        return false;
    }
    // A grant authorises exactly the command that was refused. The autonomous
    // hint paths pass 0 and therefore never consume an app's grant.
    if (opcode == 0 || g_grantOpcode != opcode) return false;
    return true;
}

bool refreshGuardMayRefresh() {
    return refreshGuardMayRefreshFor(0);
}

uint32_t refreshGuardWaitSeconds() {
    GuardLock lock;
    if (!g_haveRecord) return 0;

    if (wallUsableUnlocked()) {
        const uint32_t gone = nowUtcUnlocked() - g_lastRefreshUtc;
        return (gone >= (uint32_t)BLE_REFRESH_MIN_GAP_S)
                   ? 0
                   : (uint32_t)BLE_REFRESH_MIN_GAP_S - gone;
    }

    const uint32_t goneMs = (uint32_t)(millis() - g_refUptimeMs);
    if (goneMs >= BLE_REFRESH_MIN_GAP_MS) return 0;
    // Round up: promising "0 seconds left" while still blocked would be worse
    // than asking the user to wait one more second.
    return (BLE_REFRESH_MIN_GAP_MS - goneMs + 999UL) / 1000UL;
}

uint32_t refreshGuardElapsedSeconds() {
    GuardLock lock;
    if (!g_haveRecord) return 0;

    if (wallUsableUnlocked()) {
        return nowUtcUnlocked() - g_lastRefreshUtc;
    }

    // Lower bound only - see the invariant in the header. Reported as such, so
    // the app can word its warning as "at least N seconds ago".
    return (uint32_t)((millis() - g_refUptimeMs) / 1000UL);
}

uint8_t refreshGuardReason() {
    GuardLock lock;
    if (!g_clockKnown || !g_hasWall) return BLE_GUARD_REASON_CLOCK_UNKNOWN;
    return (nowUtcUnlocked() < g_lastRefreshUtc)
               ? BLE_GUARD_REASON_CLOCK_BEHIND
               : BLE_GUARD_REASON_CLOCK_KNOWN;
}

// ---------------------------------------------------------------- confirmation

uint8_t refreshGuardIssueToken(uint8_t forOpcode) {
    GuardLock lock;

    // Tokens are never 0, so "no token" and "token 0" cannot be confused.
    g_pendingToken = (uint8_t)(g_pendingToken + 1);
    if (g_pendingToken == 0) g_pendingToken = 1;
    g_pendingAt = millis();
    g_pendingOpcode = forOpcode;

    // A new block for the *same* command supersedes an older grant: the user is
    // being asked again. A block for a different command must not cancel an
    // approval that was already given for the first one.
    if (g_grantOpcode == forOpcode) {
        g_grantToken = 0;
    }
    return g_pendingToken;
}

bool refreshGuardArmGrant(uint8_t token) {
    GuardLock lock;

    if (g_pendingToken == 0 || token != g_pendingToken) {
        BLE_LOG("[GUARD] confirm rejected: token %u is not the live one (%u)\n",
                (unsigned)token, (unsigned)g_pendingToken);
        return false;
    }
    if (elapsed(g_pendingAt, BLE_REFRESH_GRANT_MS)) {
        g_pendingToken = 0;
        BLE_LOG("[GUARD] confirm rejected: the request had expired\n");
        return false;
    }

    // Panel safety: a confirmation may override the cooldown, but the previous
    // refresh still has to be a minimum distance away, and after a *forced*
    // refresh the next force needs the full cooldown. Otherwise a confirm loop
    // could repaint every few seconds - exactly the damage this module exists
    // to prevent.
    //
    // The floor is measured the same way as the cooldown itself: with a usable
    // wall clock it is the exact interval (a device that was unplugged for three
    // days must not have to serve 60 s of uptime before the user may confirm),
    // and without one the uptime lower bound applies, which is the safe
    // direction. Forced floor: the full gap after a forced refresh, 60 s
    // otherwise.
    if (g_haveRecord) {
        const bool useWall = wallUsableUnlocked();
        const uint32_t limitS = g_lastRefreshForced
                                    ? (uint32_t)BLE_REFRESH_MIN_GAP_S
                                    : (uint32_t)(BLE_REFRESH_FORCED_MIN_GAP_MS / 1000UL);
        if (useWall) {
            const uint32_t goneS = nowUtcUnlocked() - g_lastRefreshUtc;
            if (goneS < limitS) {
                BLE_LOG("[GUARD] confirm refused: only %lu s since the last%s refresh\n",
                        (unsigned long)goneS, g_lastRefreshForced ? " forced" : "");
                g_pendingToken = 0;
                return false;
            }
        } else {
            const uint32_t limitMs = g_lastRefreshForced ? BLE_REFRESH_MIN_GAP_MS
                                                         : BLE_REFRESH_FORCED_MIN_GAP_MS;
            if (!elapsed(g_refUptimeMs, limitMs)) {
                BLE_LOG("[GUARD] confirm refused: only %lu s (uptime bound) since "
                        "the last%s refresh\n",
                        (unsigned long)((millis() - g_refUptimeMs) / 1000UL),
                        g_lastRefreshForced ? " forced" : "");
                g_pendingToken = 0;
                return false;
            }
        }
    }

    g_grantToken = token;
    g_grantAt = millis();
    g_grantOpcode = g_pendingOpcode;
    g_pendingToken = 0;   // one confirmation, one grant
    BLE_LOG("[GUARD] user confirmed refresh %u for opcode 0x%02X, grant valid %lu s\n",
            (unsigned)token, (unsigned)g_grantOpcode,
            (unsigned long)(BLE_REFRESH_GRANT_MS / 1000UL));
    return true;
}

// ---------------------------------------------------------------- record

void refreshGuardNoteRefresh(uint8_t opcode) {
    GuardLock lock;

    // Was this the refresh the grant authorised (i.e. admitted by it)?
    const bool granted = (g_grantToken != 0) && (g_grantOpcode == opcode);
    const bool forced = granted && !allowedUnlocked();

    g_refUptimeMs = millis();
    g_haveRecord = true;
    g_lastRefreshForced = forced;

    // The grant, if any, has now been used: it authorised this refresh and
    // nothing else. The cooldown starts again from here. A grant issued for a
    // different command is left alone - this paint did not need it.
    if (granted) {
        g_grantToken = 0;
    }
    g_pendingToken = 0;

    if (g_clockKnown) {
        g_lastRefreshUtc = nowUtcUnlocked();
        g_hasWall = true;
        persist(REC_DATED, g_lastRefreshUtc, forced);
        BLE_LOG("[GUARD] refresh recorded at %lu (utc, %lu s on%s)\n",
                (unsigned long)g_lastRefreshUtc,
                (unsigned long)(g_refUptimeMs / 1000UL),
                forced ? ", forced" : "");
    } else {
        // Undated. Clearing the wall state is what keeps this honest: an older
        // timestamp from NVS describes an *older* refresh, so falling back to
        // it after a sync would overstate how long ago the newest refresh was -
        // the one direction that must never be guessed at.
        g_hasWall = false;
        g_lastRefreshUtc = 0;
        persist(REC_UNDATED, 0, forced);
        BLE_LOG("[GUARD] refresh recorded undated at %lu s uptime%s "
                "(cooldown is uptime-only)\n",
                (unsigned long)(g_refUptimeMs / 1000UL),
                forced ? ", forced" : "");
    }
}
