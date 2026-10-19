#!/usr/bin/env python3
"""Logic check for the full-refresh cooldown in refresh_guard.cpp.

The guard is the one piece of firmware whose failure can damage the panel, and
it cannot be exercised without a real e-paper panel, NVS and 180 seconds of
wall time. This script transcribes the state machine (clock anchoring, the
cumulative forward-jump clamp, the uptime fallback, the confirmation grant and
the forced-refresh floor) with a fake millis() and checks the cases that matter:

  1. a fresh device is not blocked;
  2. a dated record from a previous boot is judged by uptime until a clock
     arrives;
  3. with an exact clock the interval is exact (179 s blocked / 180 s allowed);
  4. a clock that is behind the record falls back to uptime instead of blocking
     forever, and rewrites the record so the device self-heals;
  5. repeated SYNC_TIME frames cannot fast-forward the clock beyond real uptime;
  6. a confirmation cannot force a refresh less than 60 s after the last one,
     and a second forced refresh needs the full 180 s;
  7. a grant only authorises the command it was issued for;
  8. a corrupt NVS record is treated as UNDATED, never as "no refresh ever";
  9. a legacy 12-byte v1 record still loads;
 10. a normal paint does not cancel an unrelated grant.

Keep this in sync with src/refresh_guard.cpp. Run from anywhere:

    python tools/check_refresh_guard.py
"""

from __future__ import annotations

import sys

MIN_GAP_S = 180
MIN_GAP_MS = 180_000
GRANT_MS = 120_000
FORCED_MIN_GAP_MS = 60_000
SKEW_S = 30
CLOCK_MIN_UTC = 1_000_000_000
CLOCK_MAX_UTC = 4_102_444_800

# A plausible UTC second (2023-11-14T22:13:20Z).
T0 = 1_700_000_000

OP_BEGIN = 0x02
OP_HINT = 0x05

FAILURES = 0


def check(ok: bool, label: str) -> None:
    global FAILURES
    if not ok:
        FAILURES += 1
    print(f"{'OK  ' if ok else 'FAIL'} {label}")


class Guard:
    """Mirror of the anonymous-namespace state in refresh_guard.cpp."""

    def __init__(self, now: int = 0) -> None:
        self.now = now
        self.have = False
        self.has_wall = False
        self.forced = False
        self.last_utc = 0
        self.ref_uptime = 0

        self.clock_known = False
        self.clock_utc = 0
        self.clock_uptime = 0
        self.sync_start_utc = 0
        self.sync_start_ms = 0

        self.pending = 0
        self.pending_at = 0
        self.pending_op = 0
        self.grant = 0
        self.grant_at = 0
        self.grant_op = 0

    # -- time ---------------------------------------------------------
    def advance(self, ms: int) -> None:
        self.now += ms

    # -- NVS record ---------------------------------------------------
    def load(self, kind: str, state: str = "", utc: int = 0, flags: int = 0) -> None:
        if kind == "none":
            return
        if kind == "corrupt":
            self.have = True
            self.has_wall = False
            return
        self.forced = bool(flags & 0x01)
        if state == "dated" and utc:
            self.have = True
            self.has_wall = True
            self.last_utc = utc
        elif state == "undated":
            self.have = True
            self.has_wall = False
        else:
            # Unknown state or a dated record without a timestamp.
            self.have = True
            self.has_wall = False

    # -- clock --------------------------------------------------------
    def now_utc(self) -> int:
        if not self.clock_known:
            return 0
        return self.clock_utc + (self.now - self.clock_uptime) // 1000

    def sync(self, utc: int) -> bool:
        if utc < CLOCK_MIN_UTC or utc > CLOCK_MAX_UTC:
            return False
        if not self.clock_known:
            self.sync_start_utc = utc
            self.sync_start_ms = self.now
        else:
            uptime_s = (self.now - self.sync_start_ms) // 1000
            limit = self.sync_start_utc + uptime_s + SKEW_S
            if utc > limit:
                return False
        self.clock_utc = utc
        self.clock_uptime = self.now
        self.clock_known = True
        return True

    # -- rule ---------------------------------------------------------
    def wall_usable(self) -> bool:
        return self.clock_known and self.has_wall and self.now_utc() >= self.last_utc

    def allowed(self) -> bool:
        if not self.have:
            return True
        if self.wall_usable():
            return self.now_utc() - self.last_utc >= MIN_GAP_S
        return self.now - self.ref_uptime >= MIN_GAP_MS

    def wait_s(self) -> int:
        if not self.have:
            return 0
        if self.wall_usable():
            gone = self.now_utc() - self.last_utc
            return 0 if gone >= MIN_GAP_S else MIN_GAP_S - gone
        gone = self.now - self.ref_uptime
        if gone >= MIN_GAP_MS:
            return 0
        return (MIN_GAP_MS - gone + 999) // 1000

    # -- confirmation -------------------------------------------------
    def issue(self, op: int) -> int:
        self.pending = (self.pending + 1) & 0xFF
        if self.pending == 0:
            self.pending = 1
        self.pending_at = self.now
        self.pending_op = op
        if self.grant_op == op:
            self.grant = 0
        return self.pending

    def arm(self, token: int) -> bool:
        if not self.pending or token != self.pending:
            return False
        if self.now - self.pending_at >= GRANT_MS:
            self.pending = 0
            return False
        if self.have:
            # Same yardstick as the cooldown: the exact interval when a clock is
            # usable, the uptime lower bound otherwise.
            limit_s = MIN_GAP_S if self.forced else FORCED_MIN_GAP_MS // 1000
            if self.wall_usable():
                if self.now_utc() - self.last_utc < limit_s:
                    self.pending = 0
                    return False
            else:
                limit_ms = MIN_GAP_MS if self.forced else FORCED_MIN_GAP_MS
                if self.now - self.ref_uptime < limit_ms:
                    self.pending = 0
                    return False
        self.grant = token
        self.grant_at = self.now
        self.grant_op = self.pending_op
        self.pending = 0
        return True

    def may(self, op: int) -> bool:
        if self.allowed():
            return True
        if not self.grant:
            return False
        if self.now - self.grant_at >= GRANT_MS:
            self.grant = 0
            return False
        return op != 0 and self.grant_op == op

    def note(self, op: int) -> None:
        granted = bool(self.grant) and self.grant_op == op
        forced = granted and not self.allowed()

        self.ref_uptime = self.now
        self.have = True
        self.forced = forced
        if granted:
            self.grant = 0
        self.pending = 0

        if self.clock_known:
            self.last_utc = self.now_utc()
            self.has_wall = True
        else:
            self.has_wall = False
            self.last_utc = 0


def main() -> int:
    # 1. fresh device -------------------------------------------------
    g = Guard()
    check(g.allowed(), "fresh device: cooldown not applicable")
    check(g.may(OP_BEGIN), "fresh device: BEGIN allowed")

    # 2. dated record, no clock yet -----------------------------------
    g = Guard()
    g.load("record", "dated", T0)
    g.advance(179_000)
    check(not g.allowed() and g.wait_s() == 1,
          "dated record without a clock: 179 s uptime -> blocked, 1 s left")
    g.advance(1_000)
    check(g.allowed() and g.wait_s() == 0,
          "dated record without a clock: 180 s uptime -> allowed")

    # 3. exact clock --------------------------------------------------
    g = Guard()
    g.load("record", "dated", T0)
    check(g.sync(T0), "exact clock accepted")
    g.advance(179_000)
    check(not g.allowed(), "exact clock: 179 s -> blocked")
    g.advance(1_000)
    check(g.allowed(), "exact clock: 180 s -> allowed")

    # 4. clock behind the record -> uptime fallback, then self-heal ---
    g = Guard()
    g.load("record", "dated", T0)
    check(g.sync(T0 - 3_600), "clock behind the record is still adopted")
    g.advance(100_000)
    check(not g.wall_usable() and not g.allowed() and g.wait_s() == 80,
          "clock behind: falls back to uptime (100 s -> 80 s left)")
    g.advance(80_000)
    check(g.allowed(), "clock behind: allowed after 180 s uptime (no permanent lock)")
    g.note(OP_BEGIN)
    check(g.has_wall and g.last_utc != 0,
          "clock behind: the refresh rewrites the record so the device heals")

    # 5. cumulative forward-jump clamp --------------------------------
    g = Guard()
    g.load("record", "dated", T0)
    check(g.sync(T0), "first sync accepted")
    g.advance(1_000_000)          # 1000 s of real uptime
    check(g.sync(T0 + 1_000), "sync after real uptime accepted")
    check(not g.sync(T0 + 2_000), "second sync 1000 s ahead rejected")
    g.advance(1_000_000)
    check(g.sync(T0 + 2_000), "same value accepted after another 1000 s uptime")

    # 6. confirmation floor and forced-refresh floor ------------------
    g = Guard()
    g.note(OP_BEGIN)                       # first refresh at uptime 0
    g.advance(180_000)
    g.note(OP_BEGIN)                       # normal refresh at 180 s
    g.advance(10_000)                      # 10 s since the last refresh
    tok = g.issue(OP_BEGIN)
    check(not g.arm(tok), "confirm refused 10 s after a normal refresh")
    g.advance(50_000)                      # 60 s since the last refresh
    tok = g.issue(OP_BEGIN)
    check(g.arm(tok) and g.may(OP_BEGIN), "confirm accepted at 60 s")
    g.note(OP_BEGIN)                       # this paint is forced
    check(g.forced, "the paint is recorded as forced")
    g.advance(60_000)                      # 60 s after a forced refresh
    tok = g.issue(OP_BEGIN)
    check(not g.arm(tok), "a second forced refresh is refused at 60 s")
    g.advance(120_000)                     # 180 s after the forced refresh
    tok = g.issue(OP_BEGIN)
    check(g.arm(tok), "a second forced refresh is accepted at 180 s")

    # 7. grant is bound to the refused opcode -------------------------
    g = Guard()
    g.load("record", "dated", T0)
    g.advance(60_000)
    g.sync(T0 + 60)                        # blocked (60 s < 180 s)
    check(not g.allowed(), "opcode binding: still inside the cooldown")
    tok = g.issue(OP_HINT)
    check(g.arm(tok), "opcode binding: SHOW_HINT grant armed")
    check(g.may(OP_HINT), "opcode binding: grant admits SHOW_HINT")
    check(not g.may(OP_BEGIN), "opcode binding: grant does not admit BEGIN")
    check(not g.may(0), "opcode binding: grant does not admit an automatic hint")

    # 8. an unrelated paint must not consume the grant ----------------
    g.note(OP_BEGIN)                       # image paint, admitted by cooldown? no
    # note() was called without a BEGIN grant; the SHOW_HINT grant must survive.
    check(g.grant != 0 and g.may(OP_HINT),
          "an unrelated paint leaves the SHOW_HINT grant armed")

    # 9. corrupt record is conservative -------------------------------
    g = Guard()
    g.load("corrupt")
    g.advance(179_000)
    check(not g.allowed(), "corrupt record: 179 s uptime -> blocked")
    g.advance(1_000)
    check(g.allowed(), "corrupt record: 180 s uptime -> allowed")

    # 10. legacy 12-byte record ---------------------------------------
    g = Guard()
    g.load("record", "dated", T0, flags=0)   # same fields, no flags byte
    g.sync(T0)
    check(g.has_wall and g.last_utc == T0,
          "legacy record: parsed and judged by its timestamp")
    check(not g.allowed(), "legacy record: 0 s elapsed -> blocked")
    g.advance(180_000)
    check(g.allowed(), "legacy record: 180 s elapsed -> allowed")
    check(not g.forced, "legacy record: forced flag defaults to false")

    # 11. long power-off with a correct clock -------------------------
    g = Guard()
    g.load("record", "dated", T0)
    g.advance(4 * 3600 * 1000)
    check(g.sync(T0 + 4 * 3600) and g.allowed(),
          "4 h power-off: allowed immediately once the clock is known")

    # 12. the confirmation floor uses the real interval once a clock exists
    #     (v1.3.1). A device that was unplugged for a while must not have to
    #     serve 60 s of uptime before the user may confirm, while a genuinely
    #     recent refresh must still be refused.
    g = Guard()
    g.load("record", "dated", T0)          # last refresh before this boot
    g.advance(5_000)                       # 5 s of uptime
    g.sync(T0 + 100)                       # ... but 100 s of real time
    check(not g.allowed(), "wall floor: a 100 s real interval is still inside 180 s")
    tok = g.issue(OP_BEGIN)
    check(g.arm(tok), "wall floor: 100 s real interval clears the 60 s floor "
                      "even though uptime is only 5 s")

    g = Guard()
    g.load("record", "dated", T0)
    g.advance(5_000)
    g.sync(T0 + 30)                        # 30 s of real time
    tok = g.issue(OP_BEGIN)
    check(not g.arm(tok), "wall floor: a 30 s real interval is still refused")

    g = Guard()
    g.load("record", "undated")            # no timestamp -> no wall clock
    g.advance(30_000)
    tok = g.issue(OP_BEGIN)
    check(not g.arm(tok), "uptime floor without a clock: 30 s refused")
    g.advance(30_000)
    tok = g.issue(OP_BEGIN)
    check(g.arm(tok), "uptime floor without a clock: 60 s accepted")

    g = Guard()
    g.load("record", "dated", T0, flags=1)  # last refresh was itself forced
    g.advance(5_000)
    g.sync(T0 + 120)                        # real interval 120 s < 180 s
    tok = g.issue(OP_BEGIN)
    check(not g.arm(tok),
          "wall floor: after a forced refresh the full interval is required")


    print()
    if FAILURES:
        print(f"{FAILURES} CHECK(S) FAILED")
        return 1
    print("ALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
