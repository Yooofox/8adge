#!/usr/bin/env python3
"""对 refresh_guard.cpp 中整屏刷新冷却的逻辑检查。

这个守卫是固件中唯一一处一旦失效就可能损坏面板的部分，而它离开真实的
墨水屏、NVS 和 180 秒的墙上时间就无法被检验。本脚本用一个假的 millis()
转写了这套状态机（时钟锚定、累积的前跳钳位、uptime 回退、确认授权），
并检查那些真正要紧的情形：

  1. 全新的设备不会被拦；
  2. 上次启动留下的带时间戳记录，在拿到时钟之前按 uptime 判断；
  3. 时钟精确时区间就精确（179 s 被拦 / 180 s 放行）；
  4. 落后于记录的时钟退回按 uptime 判断，而不是永远拦下去，
     并且会重写记录，从而让设备自愈；
  5. 重复的 SYNC_TIME 帧不能把时钟快进到超过真实 uptime；
  6. 用户确认后就一定放行：token 有效且未过期时不再检查距上次刷新多久
     （v1.3.2 起；旧版会在 60 s / 180 s 的下限内拒绝，确认框因此看起来无效），
     没有时钟时同样放行；
  7. 授权只对它被签发时对应的那条命令有效；
    8. 一次普通的绘制不会取消一个不相关的授权；
    9. 损坏的 NVS 记录被当作 UNDATED，绝不会当成“从未刷新过”；
   10. 旧的 12 字节 v1 记录仍能加载；
   11. 长时间断电但时钟正确时，一旦时钟已知就立即放行；
   12. 令牌本身仍受检查（v1.3.2）：未签发的 token、已消费的 token 以及
       超过 120 秒（GRANT_MS）的 token 一律拒绝。

本文件要与 src/refresh_guard.cpp 保持同步。从任意位置运行：

    python tools/check_refresh_guard.py
"""

from __future__ import annotations

import sys

MIN_GAP_S = 180
MIN_GAP_MS = 180_000
GRANT_MS = 120_000
SKEW_S = 30
CLOCK_MIN_UTC = 1_000_000_000
CLOCK_MAX_UTC = 4_102_444_800

# 一个合理的 UTC 秒数（2023-11-14T22:13:20Z）。
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
    """refresh_guard.cpp 中匿名命名空间状态的镜像。"""

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

    # -- 时间 ---------------------------------------------------------
    def advance(self, ms: int) -> None:
        self.now += ms

    # -- NVS 记录 ---------------------------------------------------
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
            # 状态未知，或者是一条带日期却没有时间戳的记录。
            self.have = True
            self.has_wall = False

    # -- 时钟 --------------------------------------------------------
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

    # -- 规则 --------------------------------------------------------
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

    # -- 确认 -------------------------------------------------
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
    # 1. 全新的设备 -------------------------------------------------
    g = Guard()
    check(g.allowed(), "fresh device: cooldown not applicable")
    check(g.may(OP_BEGIN), "fresh device: BEGIN allowed")

    # 2. 带日期的记录，尚无时钟 -----------------------------------
    g = Guard()
    g.load("record", "dated", T0)
    g.advance(179_000)
    check(not g.allowed() and g.wait_s() == 1,
          "dated record without a clock: 179 s uptime -> blocked, 1 s left")
    g.advance(1_000)
    check(g.allowed() and g.wait_s() == 0,
          "dated record without a clock: 180 s uptime -> allowed")

    # 3. 精确的时钟 --------------------------------------------------
    g = Guard()
    g.load("record", "dated", T0)
    check(g.sync(T0), "exact clock accepted")
    g.advance(179_000)
    check(not g.allowed(), "exact clock: 179 s -> blocked")
    g.advance(1_000)
    check(g.allowed(), "exact clock: 180 s -> allowed")

    # 4. 时钟落后于记录 -> 回退到 uptime，然后自愈 -------------------
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

    # 5. 累积的前跳钳位 ------------------------------------------------
    g = Guard()
    g.load("record", "dated", T0)
    check(g.sync(T0), "first sync accepted")
    g.advance(1_000_000)          # 1000 s 的真实 uptime
    check(g.sync(T0 + 1_000), "sync after real uptime accepted")
    check(not g.sync(T0 + 2_000), "second sync 1000 s ahead rejected")
    g.advance(1_000_000)
    check(g.sync(T0 + 2_000), "same value accepted after another 1000 s uptime")

    # 6. 用户确认后一定放行（不再有时间下限） ------------------------
    g = Guard()
    g.note(OP_BEGIN)                       # 第一次刷新在 uptime 0
    g.advance(180_000)
    g.note(OP_BEGIN)                       # 在 180 s 处的一次普通刷新
    g.advance(10_000)                      # 距上次刷新 10 s
    check(not g.allowed(), 'confirm: 10 s after a refresh is inside the cooldown')
    tok = g.issue(OP_BEGIN)
    check(g.arm(tok) and g.may(OP_BEGIN), 'confirm is honoured 10 s after a refresh')
    g.note(OP_BEGIN)                       # 这次绘制是被确认强制的
    check(g.forced, 'the paint is recorded as forced')
    g.advance(5_000)                       # 距强刷只有 5 s
    tok = g.issue(OP_BEGIN)
    check(g.arm(tok) and g.may(OP_BEGIN),
          'a second forced refresh is honoured 5 s after a forced one')

    # 7. 授权与被拒绝的那条 opcode 绑定 ------------------------------
    g = Guard()
    g.load("record", "dated", T0)
    g.advance(60_000)
    g.sync(T0 + 60)                        # 被拦（60 s < 180 s）
    check(not g.allowed(), "opcode binding: still inside the cooldown")
    tok = g.issue(OP_HINT)
    check(g.arm(tok), "opcode binding: SHOW_HINT grant armed")
    check(g.may(OP_HINT), "opcode binding: grant admits SHOW_HINT")
    check(not g.may(OP_BEGIN), "opcode binding: grant does not admit BEGIN")
    check(not g.may(0), "opcode binding: grant does not admit an automatic hint")

    # 8. 一次不相关的绘制不得消耗掉授权 ------------------------------
    g.note(OP_BEGIN)                       # 图像绘制，冷却会放行吗？不会
        # note() 是在没有 BEGIN 授权的情况下调用的；SHOW_HINT 的授权必须保留。
    check(g.grant != 0 and g.may(OP_HINT),
          "an unrelated paint leaves the SHOW_HINT grant armed")

    # 9. 损坏的记录被保守处理 ---------------------------------------
    g = Guard()
    g.load("corrupt")
    g.advance(179_000)
    check(not g.allowed(), "corrupt record: 179 s uptime -> blocked")
    g.advance(1_000)
    check(g.allowed(), "corrupt record: 180 s uptime -> allowed")

    # 10. 旧的 12 字节记录 --------------------------------------------
    g = Guard()
    g.load("record", "dated", T0, flags=0)   # 字段相同，但没有 flags 字节
    g.sync(T0)
    check(g.has_wall and g.last_utc == T0,
          "legacy record: parsed and judged by its timestamp")
    check(not g.allowed(), "legacy record: 0 s elapsed -> blocked")
    g.advance(180_000)
    check(g.allowed(), "legacy record: 180 s elapsed -> allowed")
    check(not g.forced, "legacy record: forced flag defaults to false")

    # 11. 时钟正确情况下的长时间断电 ---------------------------------
    g = Guard()
    g.load("record", "dated", T0)
    g.advance(4 * 3600 * 1000)
    check(g.sync(T0 + 4 * 3600) and g.allowed(),
          "4 h power-off: allowed immediately once the clock is known")

    # 12. 令牌本身仍然被检查（v1.3.2） --------------------------------
    g = Guard()
    g.load('record', 'dated', T0)
    g.sync(T0)
    g.advance(5_000)
    tok = g.issue(OP_BEGIN)
    check(not g.arm(tok + 1), 'a token that was never issued is refused')
    check(g.arm(tok), 'the live token is still armable after a rejected guess')
    check(not g.arm(tok), 'a consumed token cannot be replayed')
    tok = g.issue(OP_BEGIN)
    g.advance(GRANT_MS)
    check(not g.arm(tok), 'a token older than GRANT_MS is refused')
    tok = g.issue(OP_BEGIN)
    g.advance(GRANT_MS // 2)
    check(g.arm(tok), 'a token inside GRANT_MS is accepted')

    g = Guard()
    g.load('record', 'undated')            # 没有时间戳 -> 没有墙上时钟
    g.advance(30_000)
    check(not g.allowed(), 'no clock: 30 s uptime is inside the cooldown')
    tok = g.issue(OP_BEGIN)
    check(g.arm(tok) and g.may(OP_BEGIN), 'no clock: the confirmation is honoured')

    print()
    if FAILURES:
        print(f"{FAILURES} CHECK(S) FAILED")
        return 1
    print("ALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())