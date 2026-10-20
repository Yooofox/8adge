// ============================================================================
//  整屏刷新冷却 - 见 refresh_guard.h 了解它为何存在，以及运行时间不变式和
//  两状态 NVS 记录背后的推理。
//
//  存储：NVS 里一条 16 字节的记录（命名空间 "8adge"，键 "rec"）。Flash 每次
//  *刷新*写一次，而刷新被本模块限速为每 180 s 一次，所以最坏情况是一天 480 次
//  写入 - 一个 NVS 页能容纳远超一百条条目，因此那相当于每天几次页擦除，而擦写
//  寿命约 100k 次。是几十年，不是几年。
//
//  记录用结构体而不是若干个键，是有意为之：一个键就是一条 NVS 条目，所以一次
//  刷新只花一次写入，而且各字段绝不会互相不一致（一次刷新留下的时间戳旁边放着
//  另一次刷新的状态字节）。
// ============================================================================

#include "refresh_guard.h"

#include <Preferences.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "ble_protocol.h"
#include "config.h"

// 协议常量在 ble_protocol.h（App 读它），毫秒形式在 config.h（固件用它）。
// 二者必须一致。
static_assert(BLE_REFRESH_MIN_GAP_MS == (unsigned long)BLE_REFRESH_MIN_GAP_S * 1000UL,
              "config.h BLE_REFRESH_MIN_GAP_MS disagrees with ble_protocol.h "
              "BLE_REFRESH_MIN_GAP_S");

namespace {

constexpr char NVS_NAMESPACE[] = "8adge";   // NVS 名称上限为 15 个字符
constexpr char NVS_KEY[] = "rec";
constexpr uint32_t RECORD_MAGIC = 0x38424744UL;   // "DGB8"，小端序
constexpr uint8_t RECORD_VERSION = 2;
constexpr uint8_t RECORD_VERSION_LEGACY = 1;

// 记录的含义。记录*不存在*就是不存在的意思；*损坏*的记录被当作 UNDATED，这样
// 它永远不会被读成「从未刷新过」，否则就会允许立刻刷新。
constexpr uint8_t REC_UNDATED = 1;   // 发生过刷新，但说不出是什么时候
constexpr uint8_t REC_DATED = 2;     // ... 而且 lastRefreshUtc 就是它的 UTC 时间

// flags 的 bit 0：产生这条记录的刷新是被用户确认强制执行的（即它发生在冷却期内）。
constexpr uint8_t REC_FLAG_FORCED = 0x01;

// 16 字节。前 12 字节与 v1 记录布局兼容，因此升级会保留上次刷新时间戳，而不是
// 从盲区重新开始。
struct GuardRecord {
    uint32_t magic;
    uint8_t  version;
    uint8_t  state;
    uint8_t  flags;
    uint8_t  reserved;
    uint32_t lastRefreshUtc;   // 仅当 state == REC_DATED 时有效
    uint32_t reserved2;
};

Preferences g_prefs;
bool g_prefsReady = false;

// NimBLE 主机任务和 Arduino loop 任务都会调用本模块（一个处理 SYNC_TIME/CONFIRM，
// 另一个走绘制路径）。递归互斥锁让每次多字段更新都保持一致，包括嵌套的公开调用，
// 例如 refreshGuardMayRefreshFor() -> allowedUnlocked()。
SemaphoreHandle_t g_mux = nullptr;

struct GuardLock {
    GuardLock() { if (g_mux) xSemaphoreTakeRecursive(g_mux, portMAX_DELAY); }
    ~GuardLock() { if (g_mux) xSemaphoreGiveRecursive(g_mux); }
    GuardLock(const GuardLock &) = delete;
    GuardLock &operator=(const GuardLock &) = delete;
};

// 关于上次刷新我们知道些什么：
//   g_haveRecord      - 已知发生过某次刷新
//   g_hasWall         - ... 并且它带有精确的 UTC 时间戳
//   g_lastRefreshUtc  - 那个时间戳（仅在 g_hasWall 为真时有意义）
//   g_refUptimeMs     - 它发生时的运行时间。0 表示「本次启动之前」，这也是初始值，
//                       所以运行时间下界从第一毫秒起就是正确的。
bool     g_haveRecord = false;
bool     g_hasWall = false;
bool     g_lastRefreshForced = false;
uint32_t g_lastRefreshUtc = 0;
uint32_t g_refUptimeMs = 0;

// App 提供的时钟，锚定到我们自己的单调运行时间。g_syncStart* 是本次上电周期
// *第一次*被接受的同步，它用来约束累计的前向跳变。
bool     g_clockKnown = false;
uint32_t g_clockUtc = 0;
uint32_t g_clockUptimeMs = 0;
uint32_t g_syncStartUtc = 0;
uint32_t g_syncStartMs = 0;

// 确认握手。一份授权与被拒绝的那条命令绑定。
uint8_t  g_pendingToken = 0;
uint32_t g_pendingAt = 0;
uint8_t  g_pendingOpcode = 0;
uint8_t  g_grantToken = 0;
uint32_t g_grantAt = 0;
uint8_t  g_grantOpcode = 0;

// App 提供的时间戳的合理区间：2001-09-09 .. 2100-01-01。有意放宽 - 它只需要拒绝
// 胡言乱语（0、毫秒值、1970 年代的日期），并不负责监督时钟精度。
constexpr uint64_t CLOCK_MIN_UTC = 1000000000ULL;
constexpr uint64_t CLOCK_MAX_UTC = 4102444800ULL;

bool elapsed(uint32_t since, uint32_t limitMs) {
    // 有意使用无符号减法：在 millis() 约 49.7 天的回绕处依然正确，而本设备从不
    // 重启，很容易达到这个时长。
    return (uint32_t)(millis() - since) >= limitMs;
}

uint32_t nowUtcUnlocked() {
    if (!g_clockKnown) return 0;
    return g_clockUtc + (uint32_t)((millis() - g_clockUptimeMs) / 1000UL);
}

// 只有在 App 的时钟不早于该时间戳时，墙钟时间戳才可用。时钟落后于记录意味着记录
// （或者时钟）是错的；回退到运行时间下界能让面板至少安全地度过一个完整冷却期，
// 并让设备自行恢复，而不是永远阻塞。
bool wallUsableUnlocked() {
    return g_clockKnown && g_hasWall && nowUtcUnlocked() >= g_lastRefreshUtc;
}

bool allowedUnlocked() {
    // 没有什么需要保护的：一台出厂以来从未画过整帧的设备，没有需要恢复的波形。
    if (!g_haveRecord) return true;

    if (wallUsableUnlocked()) {
        return (nowUtcUnlocked() - g_lastRefreshUtc) >=
               (uint32_t)BLE_REFRESH_MIN_GAP_S;
    }

    // 没有可信时钟：使用头文件里的运行时间下界。
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

// ---------------------------------------------------------------- 初始化

void refreshGuardInit() {
    if (!g_mux) g_mux = xSemaphoreCreateRecursiveMutex();

    GuardLock lock;

    // 本会话还没有刷新过。0 不是占位符：它让运行时间下界读作「自启动以来的
    // millis()」，而当最新一次刷新早于本次启动时，这正是所需的下界。
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
            // 没有 NVS 时 guard 依然可用，只是无法区分断电三天和断电五秒：每次冷启动
            // 都得以运行时间坐满整个冷却期。安全，只是不太方便。
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
            // 损坏的记录绝不能读成「从未刷新过」。
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
            // 一次我们无法标注日期的刷新发生在本次启动之前。自此之后，运行时间下界
            // 就是唯一诚实的答案，而且即便 App 同步了也不会被墙钟规则取代 - 存下来的
            // 时间戳比那次刷新更早，会低估需要等待的时间。
        g_haveRecord = true;
        g_hasWall = false;
        BLE_LOG("[GUARD] last refresh is undated, cooldown is uptime-only\n");
    } else {
            // 未知状态，或者带日期却没有时间戳的记录：当作一次我们无法标注日期的刷新
            // （安全的方向）。
        g_haveRecord = true;
        g_hasWall = false;
        BLE_LOG("[GUARD] refresh record has no usable timestamp -> uptime-only\n");
    }
}

    // ---------------------------------------------------------------- 时钟

bool refreshGuardSyncClock(uint64_t unixUtc) {
    if (unixUtc < CLOCK_MIN_UTC || unixUtc > CLOCK_MAX_UTC) {
        BLE_LOG("[GUARD] rejected clock %llu (utc seconds expected)\n",
                (unsigned long long)unixUtc);
        return false;
    }

    GuardLock lock;
    const uint32_t candidate = (uint32_t)unixUtc;

    if (!g_clockKnown) {
            // 本次上电周期第一次被接受的同步：它成为约束之后每次同步的锚点。这第一个
            // 值是信任边界 - 这块板子没有别的时间源 - 但在它之后，时钟就再也不可能
            // 跑到真实运行时间前面了。
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

// ---------------------------------------------------------------- 规则

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
    // 一份授权只批准被拒绝的那一条命令。自动提示路径传 0，因此永远不会消耗
        // App 的授权。
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
        // 向上取整：在仍然被拦截时却承诺「还剩 0 秒」比请用户再多等一秒更糟。
    return (BLE_REFRESH_MIN_GAP_MS - goneMs + 999UL) / 1000UL;
}

uint32_t refreshGuardElapsedSeconds() {
    GuardLock lock;
    if (!g_haveRecord) return 0;

    if (wallUsableUnlocked()) {
        return nowUtcUnlocked() - g_lastRefreshUtc;
    }

    // 只是下界 - 见头文件里的不变式。上报时明确如此，这样 App 可以把它的警告措辞
        // 写成「至少 N 秒之前」。
    return (uint32_t)((millis() - g_refUptimeMs) / 1000UL);
}

uint8_t refreshGuardReason() {
    GuardLock lock;
    if (!g_clockKnown || !g_hasWall) return BLE_GUARD_REASON_CLOCK_UNKNOWN;
    return (nowUtcUnlocked() < g_lastRefreshUtc)
               ? BLE_GUARD_REASON_CLOCK_BEHIND
               : BLE_GUARD_REASON_CLOCK_KNOWN;
}

// ---------------------------------------------------------------- 确认

uint8_t refreshGuardIssueToken(uint8_t forOpcode) {
    GuardLock lock;

    // 令牌永远不为 0，这样「没有令牌」和「令牌 0」不会混淆。
    g_pendingToken = (uint8_t)(g_pendingToken + 1);
    if (g_pendingToken == 0) g_pendingToken = 1;
    g_pendingAt = millis();
    g_pendingOpcode = forOpcode;

    // *同一条*命令的新一次拦截会取代更早的授权：用户正在被再次询问。
        // 另一条命令的拦截绝不能取消已经给第一条命令的批准。
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

    // 用户已经看过面板警告并且选择了“仍然刷新”，所以这一次直接放行 —— 不再有额外的
    // 时间下限。早先的版本会在距上次刷新不足 60 秒（上次本身就是强刷时 180 秒）时
    // 拒绝这份确认，结果是一个“说了算却做不到”的确认框：用户点了“仍然刷新”，设备
    // 再次拒绝，屏幕毫无变化，而用户除了干等冷却自然结束之外没有任何办法。
    //
    // 让这件事保持诚实的是令牌而不是时钟：它只为一条已经被冷却拒过的请求签发、在
    // BLE_REFRESH_GRANT_MS 之后过期，并且正好被一次绘制消耗掉。所以一次确认的代价是
    // “一次重绘”，绝不会变成“重绘循环”。

    g_grantToken = token;
    g_grantAt = millis();
    g_grantOpcode = g_pendingOpcode;
    g_pendingToken = 0;   // one confirmation, one grant
    BLE_LOG("[GUARD] user confirmed refresh %u for opcode 0x%02X, grant valid %lu s\n",
            (unsigned)token, (unsigned)g_grantOpcode,
            (unsigned long)(BLE_REFRESH_GRANT_MS / 1000UL));
    return true;
}

// ---------------------------------------------------------------- 记录

void refreshGuardNoteRefresh(uint8_t opcode) {
    GuardLock lock;

    // 这次刷新是不是那份授权所批准的（即由它准入的）？
    const bool granted = (g_grantToken != 0) && (g_grantOpcode == opcode);
    const bool forced = granted && !allowedUnlocked();

    g_refUptimeMs = millis();
    g_haveRecord = true;
    g_lastRefreshForced = forced;

    // 那份授权（如果存在）现在已被用掉：它批准的就是这次刷新，别无其他。
        // 冷却从这里重新开始。为另一条命令签发的授权保持不动 - 这次绘制不需要它。
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
            // 无日期。清掉墙钟状态正是保持诚实的关键：来自 NVS 的更早时间戳描述的是
            // *更早*的一次刷新，因此同步之后再退回它就会高估最新一次刷新已经过去多久 -
            // 这是绝不能靠猜的那一个方向。
        g_hasWall = false;
        g_lastRefreshUtc = 0;
        persist(REC_UNDATED, 0, forced);
        BLE_LOG("[GUARD] refresh recorded undated at %lu s uptime%s "
                "(cooldown is uptime-only)\n",
                (unsigned long)(g_refUptimeMs / 1000UL),
                forced ? ", forced" : "");
    }
}
