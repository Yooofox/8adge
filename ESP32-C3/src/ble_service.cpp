// ============================================================================
//  BLE 图像传输 - 协议引擎与 NimBLE 胶合层。
//
//  值得保留的设计说明：
//
//  * 只有两件不能在这里做的事留在 main.cpp：绘制面板（会阻塞 15-25 s）和
//    2 分钟提示屏定时器。因此一次通过校验的传输只会置起 bleTakeRenderRequest()；
//    真正的绘制发生在 loop() 中。
//
//  * DATA 写入直接散落到 2bpp 帧缓冲，因此任何时候都不存在第二份 106 KB 的副本。
//    对 2bpp 而言这是对每个半字节组做旋转；对 1bpp 而言是把位展开到同样旋转后的
//    位置（见 ble_geometry.h）。映射以载荷字节为单位，所以一个分片可以从任意位置
//    开始、在任意位置结束，代码无需保留跨片状态。
//
//  * 完整性校验以 *块* 为单位，而不是整幅图像。App 预先为每个块声明一个 CRC32；
//    设备在块的字节到达时逐块校验，END_IMAGE 用位图回复其中失败的块。App 随后
//    只重传这些块。见下文的"为什么用块 CRC"。
//
//  * 整幅图像 CRC 在 END_IMAGE 时从帧缓冲重新计算，而不是在接收过程中累加。
//    接收侧的校验已由块 CRC 覆盖，而重新计算才使得上报的整幅图像摘要在
//    *修复之后* 仍然诚实 - 被替换掉的字节就是现在位于缓冲区中的字节，因此不会有
//    过期的滚动 CRC 与屏上的像素不一致。它的代价是对 423936 个像素遍历一遍
//    （约 50 ms），相对于 15-25 s 的整屏刷新。
//
//  * CRC 是标准的 IEEE 802.3 / zlib CRC32，与 java.util.zip.CRC32 产生的一致，
//    计算对象是载荷字节在空中传输时的原样（对 1bpp，是打包后的 1bpp 字节，
//    而不是展开后的 2bpp）。
//
//  ------------------------------------------------------------------ 为什么用块 CRC
//
//  逐包重传在我们之下已经处理好了：BLE 链路层对每个 LL 包进行确认、未确认就
//  重传，并自带 CRC，因此损坏或丢失的包永远不会到达 ATT 层。没有被保护的是它之上
//  的任何环节 - App 侧的写入返回 false 而被丢弃、手机在传输途中内存耗尽、协议栈
//  在断开时丢弃一个已排队的写入。这些都会产生一个 *空洞*，而空洞正是整幅图像 CRC
//  能发现却无法定位的东西。
//
//  因此 v1 在任何不匹配时都必须重传整个 106 KB。按实测的 9.5 kB/s，修复一个丢失的
//  512 字节写入需要 11 秒。使用 4 KB 的块后，同样的修复只需 0.4 秒，而且因为块自带
//  索引，无论它周围的字节如何移动，重传都会落在正确的偏移上。
// ============================================================================

#include "ble_service.h"

#include <NimBLEDevice.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "ble_geometry.h"
#include "ble_protocol.h"
#include "config.h"
#include "epd_398_4color.h"
#include "refresh_guard.h"

// ---------------------------------------------------------------- 模块状态

static char s_name[40] = {0};
static uint8_t *s_frame = nullptr;

static NimBLEServer *s_server = nullptr;
static NimBLECharacteristic *s_ctrl = nullptr;
static NimBLECharacteristic *s_data = nullptr;

static volatile bool s_connected = false;
static volatile bool s_everConnected = false;
static volatile uint32_t s_connCount = 0;
static volatile uint16_t s_mtu = 23;
static volatile uint16_t s_connHandle = 0;

static volatile bool s_renderPending = false;
// main.cpp 真正在绘制期间（15-25 s）置起。只有 s_renderPending 不够：
// bleTakeRenderRequest() 在绘制开始前就清掉了它，因此没有这个标志，一个速度快的
// App 就可能把下一幅图像的 BEGIN 写进此刻正推送到面板的缓冲区。
static volatile bool s_rendering = false;
static volatile uint8_t s_renderFormat = 0;
static volatile uint32_t s_renderBytes = 0;
static volatile bool s_hintRequested = false;
static volatile uint8_t s_options = 0;

// 传输状态。由 NimBLE 主机任务（data/control 回调）和 Arduino loop 任务
// （超时轮询、绘制交接、提示屏绘制）访问。单核并不能替代加锁：FreeRTOS 会在任意
// 指令处抢占，而其中若干字段构成一个逻辑整体。任何会访问其中不止一个字段的入口点
// 都会取 s_stateMux。
static SemaphoreHandle_t s_stateMux = nullptr;

struct StateLock {
    StateLock() { if (s_stateMux) xSemaphoreTakeRecursive(s_stateMux, portMAX_DELAY); }
    ~StateLock() { if (s_stateMux) xSemaphoreGiveRecursive(s_stateMux); }
    StateLock(const StateLock &) = delete;
    StateLock &operator=(const StateLock &) = delete;
};

static volatile bool s_receiving = false;
// 属于当前会话的最后一个字节/控制帧的 millis()，blePollTransferTimeout()
// 用它放弃一个卡住的传输。
static volatile uint32_t s_lastRxMs = 0;
// 在 BEGIN_IMAGE 与第一个 DATA 字节之间为 false，这样在 App 仍在准备载荷期间
// 使用长得多的 BLE_BEGIN_TIMEOUT_MS。
static bool s_firstByteSeen = false;
// 超出 BEGIN_IMAGE 中声明长度的载荷字节。它们无法被存储（每个偏移都以 s_total
// 为界），而且不再被当作致命错误：如果 App 重复发送最后一个分片，过去会因为
// ERR_OVERFLOW 丢掉一幅完全有效的图像。这里只对它们计数，并且只有当图像随后
// 校验失败时才上报 - 以 ERR_OVERFLOW 这个更精确的诊断。
static uint32_t s_strayBytes = 0;
// 来自最后一次 DATA 写入的 App 侧请求提示，使重复发送的分片能得到一个诊断，
// 而不是被无声忽略。
static volatile uint8_t s_dataHint = BLE_HINT_NONE;
// BLE_FMT_*（1 = 2bpp，2 = 1bpp）。绝不能用 bool：BLE_FMT_2BPP 是 1，用 bool 会把
// BLE_FMT_1BPP（2）塌缩成 "true"，于是每次 1bpp 传输都按 2bpp 解码并重算 CRC -
// 该传输就会永远在 END_IMAGE 失败。
static uint8_t s_format = 0;
static uint32_t s_total = 0;
static uint32_t s_received = 0;
static uint32_t s_crcDeclared = 0;      // 来自 BEGIN_IMAGE 的整幅图像 CRC
static uint8_t s_session = 0;
static uint16_t s_chunkSize = 0;
static uint8_t s_chunksInWindow = 0;

// 逐块校验。
static uint8_t s_blockShift = 0;        // 0 = 无块 CRC（v1 App）
static uint16_t s_blockCount = 0;
static uint32_t s_blockSize = 0;
static uint32_t s_blockCrcDeclared[BLE_MAX_BLOCKS];   // App 承诺的 CRC
static uint8_t s_blockBad[(BLE_MAX_BLOCKS + 7) / 8];  // 置位 = 校验失败
static uint16_t s_blockBadCount = 0;
static uint16_t s_blockIndex = 0;       // 当前正在累积的块
static uint32_t s_blockCrcRunning = 0xFFFFFFFFu;
static uint32_t s_blockBytes = 0;       // 已折入该累加器的字节数

// 修复状态（RETRY_BLOCK 已武装）。
static volatile bool s_fixing = false;
static uint16_t s_fixBlock = 0;
static uint32_t s_fixOffset = 0;        // 该块起始处的载荷偏移
static uint32_t s_fixLen = 0;           // 该块总共需要的字节数
static uint32_t s_fixNeeded = 0;        // 仍然欠缺的字节数
static uint32_t s_fixCrcRunning = 0xFFFFFFFFu;

// ---------------------------------------------------------------- CRC32 校验

// 标准 CRC-32（反射多项式 0xEDB88320，初值 0xFFFFFFFF，末尾异或
// 0xFFFFFFFF）- 与 java.util.zip.CRC32 产生的值相同。用位运算实现而非查表：
// 整幅图像两种方式都只需几十毫秒，而这样可以省下 1 KB 的表不占 RAM。
static uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc;
}

static inline uint32_t crc32Finish(uint32_t crc) { return crc ^ 0xFFFFFFFFu; }

// ---------------------------------------------------------------- 日志限流

// 最后一条被限流的日志的 millis()。见 BLE_LOG_THROTTLE_MS：窗口内的两次调用
// 会返回 false，因此重复发送的分片或持续 CRC 失败的块无法刷爆（更糟的是拖住）
// 主机任务的串口。
static uint32_t s_lastThrottledLog = 0;

static bool throttleLogDue() {
    const uint32_t now = millis();
    if ((uint32_t)(now - s_lastThrottledLog) < BLE_LOG_THROTTLE_MS) return false;
    s_lastThrottledLog = now;
    return true;
}

// ---------------------------------------------------------------- 组帧

static void pushFrame(uint8_t opcode, uint8_t seq, uint8_t status,
                      const uint8_t *payload, size_t len) {
    if (!s_ctrl) return;
    if (len > BLE_FRAME_MAX_PAYLOAD) len = BLE_FRAME_MAX_PAYLOAD;

    uint8_t frame[BLE_FRAME_MAX_LEN];
    frame[0] = opcode;
    frame[1] = seq;
    frame[2] = status;
    frame[3] = (uint8_t)len;
    if (len) memcpy(frame + BLE_FRAME_HEADER_LEN, payload, len);

    // notify(value, len) 会把帧复制进它自己的 mbuf。不要用 setValue() + notify()：
    // 那是对共享特征状态的两个独立操作，两者之间被抢占（主机任务也在这里发送）
    // 会让一个帧的字节以另一个帧的通知发出 - 造成响应/事件丢失或重复。
    // CONTROL 上没有 READ 属性，所以 setValue() 本来也没有用处。
    s_ctrl->notify(frame, len + BLE_FRAME_HEADER_LEN);
}

static void sendResponse(uint8_t opcode, uint8_t seq, uint8_t status,
                         const uint8_t *payload, size_t len) {
    pushFrame(BLE_OP_RESPONSE(opcode), seq, status, payload, len);
}

static void sendEvent(uint8_t opcode, const uint8_t *payload, size_t len) {
    pushFrame(opcode, 0, BLE_OK, payload, len);
}

static void sendStateEvent(uint8_t state, uint8_t error) {
    uint8_t p[2] = { state, error };
    sendEvent(BLE_EVT_STATE, p, sizeof(p));
}

static void sendProgress(bool blockDone) {
    uint8_t p[10];
    bleWr32(p, s_received);
    bleWr32(p + 4, s_total);
    p[8] = s_session;
    p[9] = blockDone ? BLE_PROGRESS_FLAG_BLOCK_DONE : 0;
    sendEvent(BLE_EVT_PROGRESS, p, sizeof(p));
}

// ---------------------------------------------------------------- 刷新守卫

// 拒绝一次会落在冷却期内的刷屏，并告知 App 原因。
//
// App 应当就面板的永久性损伤警告用户，并且 - 只有在用户仍然同意时 - 才在
// CONFIRM_REFRESH 中交回令牌，然后重复那条被拒绝的命令。在这期间不会有任何刷屏：
// App 的沉默意味着"不要刷屏"，对于一个无法撤销的错误来说这是安全的默认做法。
static void sendRefreshBlocked(uint8_t reqOpcode, uint8_t seq) {
    const uint8_t token = refreshGuardIssueToken(reqOpcode);

    uint8_t p[12];
    p[0] = token;
    p[1] = refreshGuardReason();
    p[2] = reqOpcode;          // 用户同意后要重试的命令
    p[3] = 0;                  // 保留字段
    bleWr16(p + 4, (uint16_t)refreshGuardWaitSeconds());
    bleWr16(p + 6, refreshGuardMinGapSeconds());
    bleWr32(p + 8, refreshGuardElapsedSeconds());

    sendResponse(reqOpcode, seq, BLE_ERR_COOLDOWN, nullptr, 0);
    sendEvent(BLE_EVT_REFRESH_BLOCKED, p, sizeof(p));

    BLE_LOG("[BLE] %s refused: refresh cooldown, %lu s left (reason %u, elapsed %lu s)\n",
            (reqOpcode == BLE_OP_BEGIN_IMAGE) ? "BEGIN_IMAGE" : "SHOW_HINT",
            (unsigned long)refreshGuardWaitSeconds(), (unsigned)refreshGuardReason(),
            (unsigned long)refreshGuardElapsedSeconds());
}

// ---------------------------------------------------------------- 传输

static uint32_t expectedLength(uint8_t format) {
    if (format == BLE_FMT_2BPP) return EPD398_COLOR_BYTES;
    if (format == BLE_FMT_1BPP) return EPD398_COLOR_BYTES / 2;
    return 0;
}

static uint16_t dataChunkSize() {
    // ATT 载荷为 MTU-3；已告知 App 不要超过这个值。
    int mtu = (int)s_mtu;
    if (mtu < 23) mtu = 23;
    int chunk = mtu - 3;
    if (chunk > BLE_DATA_CHUNK_MAX) chunk = BLE_DATA_CHUNK_MAX;
    if (chunk < 1) chunk = 1;
    return (uint16_t)chunk;
}

// 这条链路能承载的最大 CONTROL 帧值（载荷 + 4 字节头）。
// ATT 通知会被协议栈在 MTU-3 处静默截断，那会把一个长度字节承诺的字节数多于实际
// 到达字节数的帧交给 App。
static size_t attFrameLimit() {
    int mtu = (int)s_mtu;
    if (mtu < 23) mtu = 23;
    return (size_t)(mtu - 3);
}

static bool frameFits(size_t payloadLen) {
    return payloadLen + BLE_FRAME_HEADER_LEN <= attFrameLimit();
}

static void resetTransfer() {
    s_receiving = false;
    s_strayBytes = 0;
    s_format = 0;
    s_lastRxMs = 0;
    s_firstByteSeen = false;
    s_total = 0;
    s_received = 0;
    s_crcDeclared = 0;
    s_chunksInWindow = 0;
    s_blockShift = 0;
    s_blockCount = 0;
    s_blockSize = 0;
    s_blockBadCount = 0;
    s_blockIndex = 0;
    s_blockCrcRunning = 0xFFFFFFFFu;
    s_blockBytes = 0;
    s_fixing = false;
    s_fixBlock = 0;
    s_fixOffset = 0;
    s_fixLen = 0;
    s_fixNeeded = 0;
    s_fixCrcRunning = 0xFFFFFFFFu;
    memset(s_blockBad, 0, sizeof(s_blockBad));
}

static inline bool blockIsBad(uint16_t index) {
    if (index >= s_blockCount) return false;
    return (s_blockBad[index >> 3] & (1u << (index & 7))) != 0;
}

static inline void markBlock(uint16_t index, bool bad) {
    if (index >= s_blockCount) return;
    const uint8_t mask = (uint8_t)(1u << (index & 7));
    const bool wasBad = (s_blockBad[index >> 3] & mask) != 0;
    if (bad && !wasBad) {
        s_blockBad[index >> 3] |= mask;
        s_blockBadCount++;
    } else if (!bad && wasBad) {
        s_blockBad[index >> 3] &= (uint8_t)~mask;
        if (s_blockBadCount) s_blockBadCount--;
    }
}

// 在任意载荷偏移处写入一个载荷字节，并把它旋转进面板帧缓冲。
// 正常路径与修复路径共用。
static inline void storePayloadByte(uint32_t offset, uint8_t b) {
    if (s_format == BLE_FMT_2BPP) {
        const int ys = (int)(offset / BLE_IMAGE_ROW_BYTES_2BPP);
        const int xs = (int)(offset % BLE_IMAGE_ROW_BYTES_2BPP) * 4;
        bleSetCanvasPixel(s_frame, xs + 0, ys, (uint8_t)((b >> 6) & 0x03));
        bleSetCanvasPixel(s_frame, xs + 1, ys, (uint8_t)((b >> 4) & 0x03));
        bleSetCanvasPixel(s_frame, xs + 2, ys, (uint8_t)((b >> 2) & 0x03));
        bleSetCanvasPixel(s_frame, xs + 3, ys, (uint8_t)(b & 0x03));
    } else {
        // 1bpp：每像素一位，1 = 白，按 4 字节一组展开。
        const int ys = (int)(offset / BLE_IMAGE_ROW_BYTES_1BPP);
        const int xs = (int)(offset % BLE_IMAGE_ROW_BYTES_1BPP) * 8;
        for (int k = 0; k < 8; k++) {
            const bool white = ((b >> (7 - k)) & 0x01) != 0;
            bleSetCanvasPixel(s_frame, xs + k, ys, white ? EPD398_WHITE : EPD398_BLACK);
        }
    }
}

// 处理一次 DATA 写入。运行在 NimBLE 主机任务中。
//
// 两种模式：正常接收（字节计入 s_received 并滚入当前块）和块修复（字节写到已武装
// 的块的固定偏移）。两者都经由 storePayloadByte()，因此旋转和格式处理只存在一份。
static void handleData(const uint8_t *d, size_t n) {
    if (n == 0) return;

    StateLock lock;

    if (!s_receiving) {
        // 传输之外的多余写入。忽略它而不是把链路拆掉 - App 可能只是与一次 ABORT
        // 抢跑，而在 END_IMAGE 落地时已经排队的那个分片不应该让该 App 丢掉图像。
        // 也不做无条件日志：一个在 END_IMAGE 之后继续流水发送的 App 会刷爆串口。
        // s_dataHint 是下一次 END_IMAGE 用来解释自身的东西，而下面那条被限流的日志
        // 覆盖了真正会发生的情况 - 上一幅图像正在绘制期间有数据到达。（重复发送的
        // 分片可以让 BLE_DATA_CHUNK_MAX 窗口在限流之后仍然保持打开，因此一次进度上报
        // 并不能保证 App 已经收到任何东西。）
        if (s_renderPending || s_rendering) {
            s_dataHint = BLE_HINT_DATA_AFTER_END;
            if (throttleLogDue()) {
                BLE_LOG("[BLE] DATA during the render of the previous image ignored "
                        "(the app is pipelining past its %u-chunk window)\n",
                        (unsigned)BLE_WINDOW_CHUNKS);
            }
        }
        return;
    }
    if (!s_frame) {
        // 防御性处理：没有帧缓冲时 BEGIN_IMAGE 会拒绝，因此这里不可能存在会话。
        // 只对这些字节计数，而不是把会话拆掉；下面的校验随后会自行失败。
        s_strayBytes += (uint32_t)n;
        return;
    }

    // 当前会话接受的任何字节都证明 App 还在。
    s_lastRxMs = millis();
    s_firstByteSeen = true;

    if (s_fixing) {
        // 修复：填充已武装的块，忽略超出其长度的任何内容，这样之前残留的、仍在
        // 传输中的分片就不会破坏下一个块。
        //
        // 注意：s_received 在这里故意不动。它统计的是已接收的载荷字节，而一次修复
        // 替换的是已经被计入的字节；递增它会让总数超过 s_total，使下一次 END_IMAGE
        // 看起来像超长。
        // 注意：目标偏移随已折入该块的字节推进，而不是随分片的局部下标推进。
        // 直接用 `i` 会把每个分片都钉在块的起点，于是任何大于一个分片的块最终
        // 都只持有它第一个分片的重复。
        size_t take = n;
        if (take > s_fixNeeded) take = (size_t)s_fixNeeded;
        const uint32_t consumed = s_fixLen - s_fixNeeded;
        for (size_t i = 0; i < take; i++) {
            storePayloadByte(s_fixOffset + consumed + (uint32_t)i, d[i]);
        }
        s_fixCrcRunning = crc32Update(s_fixCrcRunning, d, take);
        s_fixNeeded -= (uint32_t)take;

        if (s_fixNeeded == 0) {
            const bool bad = crc32Finish(s_fixCrcRunning) != s_blockCrcDeclared[s_fixBlock];
            markBlock(s_fixBlock, bad);
            BLE_LOG("[BLE] block %u repaired -> %s\n", (unsigned)s_fixBlock,
                    bad ? "STILL BAD" : "ok");
            s_fixing = false;
            sendStateEvent(BLE_STATE_RECEIVING, BLE_OK);
            sendProgress(true);
        }
        return;
    }

    // 超出声明长度的字节被忽略，而不是致命错误：它们无处可存（每个载荷偏移都以
    // s_total 为界），而一个重复发送分片的 App 过去会因此丢掉一幅已经完整且正确的
    // 图像。这里对它们计数，使 END_IMAGE 在图像同样校验不通过时能说出真正的问题。
    uint32_t take = (s_received < s_total) ? (s_total - s_received) : 0;
    if (take > (uint32_t)n) take = (uint32_t)n;
    if (take < (uint32_t)n) s_strayBytes += (uint32_t)n - take;

    // 一趟循环：存储字节、把它滚入当前块，并在它最后一个字节落地的那一刻关闭该块。
    //
    // 这必须是单个循环。把块的记账放到第二趟（早期版本的做法）意味着 CRC 累加器
    // 在发现任何边界之前就吃掉了整个分片的字节，于是只要块边界落在分片内部，边界
    // *之后* 的字节就会被折进前一个块的 CRC，从那里开始的每个块都会报告不匹配。
    // 使用默认的 512 字节分片和 4096 字节的块时，边界总是落在分片边缘，这正是这个
    // 问题在测试一个不能整除的分片大小之前一直没被发现的原因。
    for (size_t i = 0; i < take; i++) {
        const uint32_t offset = s_received + (uint32_t)i;
        storePayloadByte(offset, d[i]);
        if (s_blockShift == 0) continue;

        // 上一个边界可能正停在分片边缘。在新分片的第一个字节处重置累加器，
        // 而不是在边界本身处重置：如果整整一次写入都丢了，正是完整的
        // s_blockBytes 让 closeMissingBlocks() 能指明丢失的块。
        if (s_blockBytes == s_blockSize) {
            s_blockCrcRunning = 0xFFFFFFFFu;
            s_blockBytes = 0;
        }

        s_blockCrcRunning = crc32Update(s_blockCrcRunning, &d[i], 1);
        s_blockBytes++;
        // 一幅图像的最后一个块通常是短的：在最后一个已声明的字节处关闭它，
        // 而不是等一个永远不来的完整块。
        if (s_blockBytes == s_blockSize || (offset + 1) == s_total) {
            const bool bad =
                crc32Finish(s_blockCrcRunning) != s_blockCrcDeclared[s_blockIndex];
            markBlock(s_blockIndex, bad);
            // 限流：无论哪种情况位图和 s_blockBadCount 都保持精确，
            // 只是跳过串口这一行。
            if (bad && throttleLogDue()) {
                BLE_LOG("[BLE] block %u FAILED crc %08lX != %08lX\n",
                        (unsigned)s_blockIndex,
                        (unsigned long)crc32Finish(s_blockCrcRunning),
                        (unsigned long)s_blockCrcDeclared[s_blockIndex]);
            }
            s_blockIndex++;
            // 如果这是分片的结尾，这里故意不重置 s_blockCrcRunning/s_blockBytes；
            // 见上面的重置。
        }
    }
    s_received += take;

    // App 手上还留着的东西都在传输中：一个超出声明长度才到达的分片意味着它的
    // END_IMAGE 可能已在路上。
    s_dataHint = s_strayBytes ? BLE_HINT_DATA_IGNORED
                              : ((s_received >= s_total) ? BLE_HINT_DATA_AFTER_END
                                                         : BLE_HINT_NONE);

    s_chunksInWindow++;
    if (s_received >= s_total || s_chunksInWindow >= BLE_WINDOW_CHUNKS) {
        s_chunksInWindow = 0;
        sendProgress(false);
    }
}

// 从帧缓冲重建载荷摘要。
//
// 通过 bleGetCanvasPixel() 把画布读回来，才使这在一次修复之后仍然诚实，而且重新
// 打包在两个方向上都是精确的：2bpp 逐字存储四个颜色码，1bpp 载荷里只可能出现黑与白，
// 所以按阈值还原成一位不会有任何损失。
static uint32_t recomputeWholeCrc() {
    if (!s_frame) return 0;
    uint32_t crc = 0xFFFFFFFFu;
    uint8_t out[BLE_IMAGE_ROW_BYTES_2BPP];
    int used = 0;

    if (s_format == BLE_FMT_2BPP) {
        for (int y = 0; y < BLE_IMAGE_HEIGHT; y++) {
            used = 0;
            for (int x = 0; x < BLE_IMAGE_WIDTH; x += 4) {
                const uint8_t packed =
                    (uint8_t)((bleGetCanvasPixel(s_frame, x + 0, y) << 6) |
                              (bleGetCanvasPixel(s_frame, x + 1, y) << 4) |
                              (bleGetCanvasPixel(s_frame, x + 2, y) << 2) |
                              (bleGetCanvasPixel(s_frame, x + 3, y)));
                out[used++] = packed;
            }
            crc = crc32Update(crc, out, (size_t)used);
        }
    } else {
        for (int y = 0; y < BLE_IMAGE_HEIGHT; y++) {
            used = 0;
            for (int x = 0; x < BLE_IMAGE_WIDTH; x += 8) {
                uint8_t b = 0;
                for (int k = 0; k < 8; k++) {
                    if (bleGetCanvasPixel(s_frame, x + k, y) == EPD398_WHITE) {
                        b = (uint8_t)(b | (0x80 >> k));
                    }
                }
                out[used++] = b;
            }
            crc = crc32Update(crc, out, (size_t)used);
        }
    }
    return crc32Finish(crc);
}

// ---------------------------------------------------------------- 请求

// 设备能力。布局见 docs/BLE_PROTOCOL.md §5.2；image_width/height 是 App 必须
// 生成的 *画布*，panel_width/height 是玻璃面板本身的尺寸，rotation 是设备在两者
// 之间施加的旋转（仅供参考 - App 从不预先旋转）。
//
// v3 把刷新冷却追加在可变长度设备名 *之后*，因此 v2 解析器已经知道的每个偏移都保持
// 原位：先通过 device_name_len 读出名字，再读后面那两个字节。
static void handleGetInfo(uint8_t seq) {
    // 按固定块加上本固件能构建出的最长名字（s_name 为 40 字节），再加末尾的 v3 字段
    // 来定尺寸。无论哪种情况回复都远低于 BLE_FRAME_MAX_PAYLOAD；这只是消除名字一旦被
    // 改长时出现的静默溢出。
    uint8_t p[96];
    size_t o = 0;
    const size_t nameLen = strlen(s_name);

    // 39 个固定字节 + 名字。如果协商出的 MTU 无法承载该回复，就直接说明，
    // 而不是让协议栈把它截断成一个畸形帧。
    if (!frameFits(39 + nameLen)) {
        BLE_LOG("[GET_INFO] MTU %u too small for a %u-byte reply\n",
                (unsigned)s_mtu, (unsigned)(39 + nameLen));
        sendResponse(BLE_OP_GET_INFO, seq, BLE_ERR_MTU, nullptr, 0);
        return;
    }

    p[o++] = BLE_PROTOCOL_VERSION;
    p[o++] = FW_VERSION_MAJOR;
    p[o++] = FW_VERSION_MINOR;
    p[o++] = FW_VERSION_PATCH;
    bleWr16(p + o, (uint16_t)BLE_IMAGE_WIDTH);   o += 2;
    bleWr16(p + o, (uint16_t)BLE_IMAGE_HEIGHT);  o += 2;
    bleWr16(p + o, (uint16_t)EPD398_WIDTH);      o += 2;
    bleWr16(p + o, (uint16_t)EPD398_HEIGHT);     o += 2;
    p[o++] = EPD398_BPP;
    p[o++] = 4;
    p[o++] = EPD398_BLACK;
    p[o++] = EPD398_WHITE;
    p[o++] = EPD398_YELLOW;
    p[o++] = EPD398_RED;
    bleWr32(p + o, (uint32_t)EPD398_COLOR_BYTES);       o += 4;  // BLE_FMT_2BPP
    bleWr32(p + o, (uint32_t)(EPD398_COLOR_BYTES / 2)); o += 4;  // BLE_FMT_1BPP
    bleWr16(p + o, s_mtu);              o += 2;
    bleWr16(p + o, dataChunkSize());    o += 2;
    bleWr16(p + o, BLE_WINDOW_CHUNKS);  o += 2;
    p[o++] = (uint8_t)BLE_ROTATION;
    // v2 能力块：接受哪些块 shift，以及失败位图能容纳多少个块。
    // v1 App 直接忽略这些多余字节。
    p[o++] = BLE_BLOCK_SHIFT_MIN;
    p[o++] = BLE_BLOCK_SHIFT_MAX;
    p[o++] = (uint8_t)BLE_MAX_BLOCKS;
    p[o++] = (uint8_t)nameLen;
    memcpy(p + o, s_name, nameLen);
    o += nameLen;

    // v3：整屏刷新之间的最小间隔。App 需要在发送任何东西之前就拿到它，
    // 以便提前而不是在被拒绝之后才告诉用户"面板在 N 秒前刚刷新过"。
    bleWr16(p + o, refreshGuardMinGapSeconds());
    o += 2;

    sendResponse(BLE_OP_GET_INFO, seq, BLE_OK, p, o);
}

static void handleBeginImage(uint8_t seq, const uint8_t *p, size_t len) {
    if (len < 12) { sendResponse(BLE_OP_BEGIN_IMAGE, seq, BLE_ERR_LENGTH, nullptr, 0); return; }
    if (s_renderPending || s_rendering) {
        sendResponse(BLE_OP_BEGIN_IMAGE, seq, BLE_ERR_BUSY, nullptr, 0);
        return;
    }
    if (s_receiving) {
        // 一个有效的新 BEGIN 会取代未完成或已失败的会话：App 明确地重新开始
        // （文档：ERR_CRC 之后，从 BEGIN 重试）。在这里记录日志，但在新请求通过校验
        // 和冷却闸门之前，保持旧会话存活。
        BLE_LOG("[BLE] BEGIN supersedes unfinished session (%lu/%lu bytes)\n",
                (unsigned long)s_received, (unsigned long)s_total);
    }

    const uint8_t format = p[0];
    const uint8_t blockShift = p[2];
    const uint32_t total = bleRd32(p + 4);
    const uint32_t crc   = bleRd32(p + 8);

    // flags 与 reserved 在文档中规定必须为 0。在这里接受垃圾值会把未来的帧版本
    // 静默地按当前版本重新解释。
    if (p[1] != 0 || p[3] != 0) {
        sendResponse(BLE_OP_BEGIN_IMAGE, seq, BLE_ERR_SIZE, nullptr, 0);
        return;
    }

    const uint32_t want = expectedLength(format);
    if (want == 0) { sendResponse(BLE_OP_BEGIN_IMAGE, seq, BLE_ERR_FORMAT, nullptr, 0); return; }
    if (total != want) { sendResponse(BLE_OP_BEGIN_IMAGE, seq, BLE_ERR_SIZE, nullptr, 0); return; }
    if (!s_frame) { sendResponse(BLE_OP_BEGIN_IMAGE, seq, BLE_ERR_MEM, nullptr, 0); return; }

    if (blockShift != 0 &&
        (blockShift < BLE_BLOCK_SHIFT_MIN || blockShift > BLE_BLOCK_SHIFT_MAX)) {
        sendResponse(BLE_OP_BEGIN_IMAGE, seq, BLE_ERR_SIZE, nullptr, 0);
        return;
    }

    // 刷新冷却。在这里检查而不是在 END_IMAGE 处，这样被拒绝不付出任何代价 -
    // 否则就要传输 106 KB 之后才被告知不行。用户先前确认过的授权在这里会被接受，
    // 并一直保持武装直到面板真正被绘制，因此一次因 CRC 失败的传输不会让用户确认两次。
    if (!refreshGuardMayRefreshFor(BLE_OP_BEGIN_IMAGE)) {
        sendRefreshBlocked(BLE_OP_BEGIN_IMAGE, seq);
        return;
    }

    resetTransfer();
    s_receiving = true;
    s_lastRxMs = millis();
    s_firstByteSeen = false;
    s_format = format;
    s_total = total;
    s_crcDeclared = crc;
    s_chunkSize = dataChunkSize();
    s_session++;
    // 新会话从干净的状态开始，包括下一次 ERR_STATE 的 END_IMAGE 会携带的诊断信息。
    s_dataHint = BLE_HINT_NONE;

    s_blockShift = blockShift;
    if (blockShift != 0) {
        s_blockSize = 1u << blockShift;
        s_blockCount = (uint16_t)((total + s_blockSize - 1) / s_blockSize);
        if (s_blockCount > BLE_MAX_BLOCKS) {
            resetTransfer();
            sendResponse(BLE_OP_BEGIN_IMAGE, seq, BLE_ERR_SIZE, nullptr, 0);
            return;
        }
        // END_IMAGE 的失败回复携带 ceil(count/8) 字节的位图。如果它装不进协商出的
        // MTU，App 就永远无法知道该重传哪些块，所以在传输任何数据之前就拒绝。
        if (!frameFits(10 + (size_t)((s_blockCount + 7) / 8))) {
            resetTransfer();
            BLE_LOG("[BLE] MTU %u too small for a %u-block failure bitmap\n",
                    (unsigned)s_mtu, (unsigned)s_blockCount);
            sendResponse(BLE_OP_BEGIN_IMAGE, seq, BLE_ERR_MTU, nullptr, 0);
            return;
        }
        // 在 BLOCK_CRCS 到达之前，每个块都算作未校验。声明 CRC 仍为初始值 0 的块
        // 也会在比较中判为失败，因此忘记发送 BLOCK_CRCS 的 App 得到的是 ERR_BLOCKS，
        // 而不是被静默接受的图像。
        memset(s_blockCrcDeclared, 0, sizeof(s_blockCrcDeclared));
    }

    uint8_t r[12];
    bleWr16(r, s_chunkSize);
    bleWr16(r + 2, BLE_WINDOW_CHUNKS);
    bleWr32(r + 4, s_total);
    r[8] = s_session;
    r[9] = s_format;
    r[10] = s_blockShift;
    r[11] = (uint8_t)s_blockCount;
    sendResponse(BLE_OP_BEGIN_IMAGE, seq, BLE_OK, r, sizeof(r));

    BLE_LOG("[BLE] BEGIN format=%u total=%lu chunk=%u session=%u blockShift=%u blocks=%u\n",
            (unsigned)format, (unsigned long)total, (unsigned)s_chunkSize,
            (unsigned)s_session, (unsigned)s_blockShift, (unsigned)s_blockCount);
    sendStateEvent(BLE_STATE_RECEIVING, BLE_OK);
}

static void handleBlockCrcs(uint8_t seq, const uint8_t *p, size_t len) {
    if (len < 4) { sendResponse(BLE_OP_BLOCK_CRCS, seq, BLE_ERR_LENGTH, nullptr, 0); return; }
    if (!s_receiving) { sendResponse(BLE_OP_BLOCK_CRCS, seq, BLE_ERR_STATE, nullptr, 0); return; }
    if (s_blockShift == 0) {
        // App 在 BEGIN 里请求了块 CRC 吗？没有 - 它没有请求，所以这一帧没有可以依附的
        // 对象。
        sendResponse(BLE_OP_BLOCK_CRCS, seq, BLE_ERR_STATE, nullptr, 0);
        return;
    }
    if (s_received != 0) {
        // 必须在第一个 DATA 字节之前到达，否则设备已经关闭了一些无法追溯校验的块。
        sendResponse(BLE_OP_BLOCK_CRCS, seq, BLE_ERR_STATE, nullptr, 0);
        return;
    }

    const uint16_t blockSize = bleRd16(p);
    const uint16_t count = bleRd16(p + 2);
    if (blockSize != s_blockSize || count != s_blockCount ||
        len < (size_t)(4 + 4 * count)) {
        sendResponse(BLE_OP_BLOCK_CRCS, seq, BLE_ERR_SIZE, nullptr, 0);
        return;
    }

    for (uint16_t i = 0; i < count; i++) {
        s_blockCrcDeclared[i] = bleRd32(p + 4 + 4 * i);
    }
    s_lastRxMs = millis();
    const uint8_t echo = (uint8_t)count;
    sendResponse(BLE_OP_BLOCK_CRCS, seq, BLE_OK, &echo, 1);
    BLE_LOG("[BLE] %u block CRCs accepted (block size %lu)\n",
            (unsigned)count, (unsigned long)s_blockSize);
}

static void handleRetryBlock(uint8_t seq, const uint8_t *p, size_t len) {
    if (len < 2) { sendResponse(BLE_OP_RETRY_BLOCK, seq, BLE_ERR_LENGTH, nullptr, 0); return; }
    if (!s_receiving || s_blockShift == 0) {
        sendResponse(BLE_OP_RETRY_BLOCK, seq, BLE_ERR_STATE, nullptr, 0);
        return;
    }
    const uint16_t index = bleRd16(p);
    // reserved 必须为 0，与 BEGIN_IMAGE 的 flags/reserved 检查完全一致：在那里
    // 接受垃圾值会把未来的帧版本静默地按当前版本重新解释。仅在字段存在时才检查，
    // 因此省略该字段的 App 发来的 2 字节帧仍然可用。
    if (len >= 4 && bleRd16(p + 2) != 0) {
        sendResponse(BLE_OP_RETRY_BLOCK, seq, BLE_ERR_SIZE, nullptr, 0);
        return;
    }
    if (index >= s_blockCount) {
        sendResponse(BLE_OP_RETRY_BLOCK, seq, BLE_ERR_SIZE, nullptr, 0);
        return;
    }
    if (!blockIsBad(index)) {
        // 已经是好的 - 无事可做，但不要让 App 把它当成失败。响应携带零长度，
        // 让它知道可以跳过。
        const uint8_t r[4] = {0, 0, 0, 0};
        sendResponse(BLE_OP_RETRY_BLOCK, seq, BLE_OK, r, sizeof(r));
        return;
    }

    // 在上一次修复仍未完成时重新武装（docs §5.9：一次一个块）会静默丢弃该块
    // 已收到的字节。块仍被标记为坏，也没有任何损坏，但 App 丢掉了它发送的内容 -
    // 所以改为拒绝，这是一次往返，而且不会被误认为进展。
        if (s_fixing) {
            BLE_LOG("[BLE] RETRY_BLOCK %u refused: block %u still has %lu of %lu bytes outstanding\n",
                (unsigned)index, (unsigned)s_fixBlock,
                (unsigned long)s_fixNeeded, (unsigned long)s_fixLen);
            sendResponse(BLE_OP_RETRY_BLOCK, seq, BLE_ERR_BUSY, nullptr, 0);
            return;
        }
        s_fixBlock = index;
    s_fixOffset = (uint32_t)index * s_blockSize;
    uint32_t remain = s_total - s_fixOffset;
    s_fixLen = (remain < s_blockSize) ? remain : s_blockSize;
    s_fixNeeded = s_fixLen;
    s_fixCrcRunning = 0xFFFFFFFFu;
    s_fixing = true;
    s_lastRxMs = millis();

    // App 接下来必须发送的字节数，这样它不必自己推导出那个短的末块。
    uint8_t r[4];
    bleWr16(r, index);
    bleWr16(r + 2, (uint16_t)s_fixNeeded);
    sendResponse(BLE_OP_RETRY_BLOCK, seq, BLE_OK, r, sizeof(r));
    sendStateEvent(BLE_STATE_FIXING, BLE_OK);
    BLE_LOG("[BLE] RETRY block %u (%lu bytes)\n",
            (unsigned)index, (unsigned long)s_fixNeeded);
}

// 处理那些字节始终没有到达的块。
//
// handleData() 中的逐分片重置会标记字节缺失或错位的块；如果丢失的内容包含最后一个
// 块，就永远没有东西去关闭它，END_IMAGE 也无法指明它。在这里关闭尾部（先关闭当前
// 不完整的块，再关闭其余每一个块）会把这些块标记为坏，使 ERR_BLOCKS 携带一份 App 可以
// 修复的位图。它不改变 s_received：那是一个线缆字节计数器，而被修复的块替换的是
// 已经被计入的字节。
static void closeMissingBlocks() {
    if (s_blockShift == 0 || s_blockCount == 0) return;
    if (s_blockIndex >= s_blockCount) return;

    if (s_blockBytes != 0) {
        // 不完整的块无法与针对完整块声明的 CRC 匹配。
        markBlock(s_blockIndex, true);
        s_blockIndex++;
    }
    while (s_blockIndex < s_blockCount) {
        markBlock(s_blockIndex, true);
        s_blockIndex++;
    }
    s_blockCrcRunning = 0xFFFFFFFFu;
    s_blockBytes = 0;
}

static void handleEndImage(uint8_t seq, const uint8_t *p, size_t len) {
    if (len < 4) { sendResponse(BLE_OP_END_IMAGE, seq, BLE_ERR_LENGTH, nullptr, 0); return; }
    if (!s_receiving) {
        // 没有会话可以校验。有两种 App 会走到这里：一种与 ABORT 抢跑，另一种把
        // 它的 DATA 直接流水写进刚结束那幅图像的绘制过程 - 日志限流是按
        // BLE_DATA_CHUNK_MAX 窗口计的，因此一条快速的链路会让该窗口远在其后仍然
        // 保持打开，而这就是这种 App 唯一能得到的答复。hint 字节说明了是哪一种。
        const uint8_t hint = s_dataHint;   // volatile -> 拷出一份普通副本用于传递
        sendResponse(BLE_OP_END_IMAGE, seq, BLE_ERR_STATE, &hint, 1);
        return;
    }

    // 结束一次传输也是活动：App 可能需要一段时间来检查 ERR_BLOCKS 并发起
    // RETRY_BLOCK，因此停滞计时在这里重新开始，而不是在它做决定时就到期。
    s_lastRxMs = millis();

    const uint32_t declared = bleRd32(p);
    // 在任何 resetTransfer() 之前就取好，这样成功日志和失败分支报告的是同一个
    // 数字。
    const uint32_t stray = s_strayBytes;
    // 按可能的最大失败回复定尺寸：8 字节的 CRC/字节数、两个 ERR_BLOCKS 计数器，
    // 以及 BLE_MAX_BLOCKS 对应的 16 字节位图。
    uint8_t r[4 + 4 + 2 + (BLE_MAX_BLOCKS + 7) / 8];
    // 成功回复只携带这些，不多不少：文档规定的成功载荷固定为 8 字节，因此
    // 绝不能发送 sizeof(r)（26）。
    const size_t OK_PAYLOAD_LEN = 8;

    sendStateEvent(BLE_STATE_VERIFYING, BLE_OK);

    // 关闭链路丢掉的任何块，这样失败位图才能指明它。
    closeMissingBlocks();

    // 整幅图像摘要，从最终真正落在缓冲区里的内容重新计算。
    // 在这里算（而不是在接收过程中累加）正是让上报值在一次修复之后依然正确的
    // 原因。
    const uint32_t actual = recomputeWholeCrc();

    const bool complete = (s_received == s_total);
    const bool blocksEnabled = (s_blockShift != 0);
    // BEGIN 和 END 里的 CRC 描述的是同一份载荷；如果它们不一致，说明 App 糊涂了
    // （或者帧已损坏），因此在触碰面板之前就失败。无论哪种情况都会上报重新计算的
    // 值。
    const bool declaredOk = (s_crcDeclared == declared);
    // 每个块都存在且已通过校验。s_blockIndex 到达 s_blockCount 意味着所有块都被
    // 关闭了 - 通常是被收到的字节关闭，否则是被 closeMissingBlocks() +
    // RETRY_BLOCK 关闭。被修复的短块会填满帧缓冲，因此线上的字节数不再必须等于
    // s_total。
    const bool blocksOk = blocksEnabled &&
                          (s_blockBadCount == 0) &&
                          (s_blockIndex == s_blockCount);

    bool ok;
    if (blocksEnabled) {
        // 逐块 CRC 严格强于一个覆盖全部内容的摘要，所以由它们说了算。整幅图像
        // CRC 仍然会上报，供 App 交叉核对，但那里若不一致就意味着 bug，而不是
        // 链路不好。超过 total_len 的字节已在上面被忽略，不可能让一幅已通过校验的
        // 图像失败，因此即使存在这些字节，完全通过校验的载荷仍会被接受。
        ok = blocksOk && declaredOk && (actual == declared);
    } else {
        ok = complete && declaredOk && (actual == declared);
    }

    if (ok) {
        const uint8_t doneFormat = s_format;
        const uint32_t doneBytes = s_total;
        resetTransfer();
        s_renderFormat = doneFormat;
        s_renderBytes = doneBytes;
        s_renderPending = true;

        bleWr32(r, doneBytes);
        bleWr32(r + 4, actual);
        sendResponse(BLE_OP_END_IMAGE, seq, BLE_OK, r, OK_PAYLOAD_LEN);
        BLE_LOG("[BLE] transfer OK: %lu bytes, crc %08lX\n",
                (unsigned long)doneBytes, (unsigned long)actual);
                if (stray) {
                    // 可以容忍，但 App 的分片算术值得修一修：下一幅图像可能
                    // 就没这么幸运了。
                    BLE_LOG("[BLE]   note: %lu byte(s) past total_len were ignored\n",
                            (unsigned long)stray);
                }
                return;
    }

    // 失败。上报足够的信息，让 App 无需第二次往返就能在"重传一些块"和
    // "从头开始"之间做决定。
    bleWr32(r, actual);
    bleWr32(r + 4, s_received);

    uint8_t status;
    size_t plen;
    if (stray != 0) {
        // 有字节超过声明长度才到达。它们已被忽略（本来也无处可存），而载荷也没能
        // 通过校验，所以可疑的是 App 的组帧 - 这比给出一份针对错位数据流的位图
        // 更有用，而且会话仍然可以重试。
        plen = 8;
        status = BLE_ERR_OVERFLOW;
        BLE_LOG("[BLE] END: %lu bytes over total_len (%lu/%lu), crc %08lX (want %08lX)\n",
                    (unsigned long)stray, (unsigned long)s_received,
                (unsigned long)s_total, (unsigned long)actual,
                (unsigned long)declared);
    } else if (blocksEnabled && s_blockBadCount > 0) {
        const size_t bitmapLen = (s_blockCount + 7) / 8;
        r[8] = (uint8_t)s_blockCount;
        r[9] = (uint8_t)s_blockBadCount;
        memcpy(r + 10, s_blockBad, bitmapLen);
        plen = 10 + bitmapLen;
        status = BLE_ERR_BLOCKS;
        BLE_LOG("[BLE] END: %u/%u blocks bad, %lu/%lu bytes, crc %08lX (want %08lX)\n",
                (unsigned)s_blockBadCount, (unsigned)s_blockCount,
                (unsigned long)s_received, (unsigned long)s_total,
                (unsigned long)actual, (unsigned long)declared);
    } else {
        plen = 8;
        status = BLE_ERR_CRC;
        BLE_LOG("[BLE] END: %lu/%lu bytes, crc %08lX (want %08lX)\n",
                (unsigned long)s_received, (unsigned long)s_total,
                (unsigned long)actual, (unsigned long)declared);
    }
    sendResponse(BLE_OP_END_IMAGE, seq, status, r, plen);

    // 保持在接收状态，这样 App 不必重新 BEGIN_IMAGE 就能 RETRY_BLOCK。
    // ABORT 或新的 BEGIN_IMAGE 会清掉它。正是这个事件让跟踪 EVT_STATE 的 App
    // 不会永远停在 VERIFYING。
    sendStateEvent(BLE_STATE_RECEIVING, BLE_OK);
}

// App 的时钟，每次连接采纳一次（或 App 想纠正时随时采纳）。这是设备唯一的
// 时间来源：它从不连接任何东西，而它的 RTC 熬不过断电。该值被锚定到设备自己的
// 单调运行时间上，因此从此以后时间会自行向前走，冷却也就变成精确比较而不是一个
// 界限。
static void handleSyncTime(uint8_t seq, const uint8_t *p, size_t len) {
    if (len < 8) { sendResponse(BLE_OP_SYNC_TIME, seq, BLE_ERR_LENGTH, nullptr, 0); return; }

    // 自纪元以来的 UTC 秒数，小端，64 位。用 64 位而不是 32 位，是为了让那个
    // 经典错误 - 直接发送 System.currentTimeMillis() 的毫秒数 - 不会被静默地
    // 截断成一个碰巧看起来像合理日期的值。
    uint64_t utc = 0;
    for (int i = 7; i >= 0; i--) utc = (utc << 8) | p[i];

    if (!refreshGuardSyncClock(utc)) {
        sendResponse(BLE_OP_SYNC_TIME, seq, BLE_ERR_SIZE, nullptr, 0);
        return;
    }

    uint8_t r[8];
    bleWr32(r, refreshGuardLastRefreshUtc());   // 0 = 无，或者无法确定日期
    bleWr16(r + 4, refreshGuardMinGapSeconds());
    r[6] = 1;   // 从这里开始时钟是已知的
    r[7] = 0;   // 保留字段
    sendResponse(BLE_OP_SYNC_TIME, seq, BLE_OK, r, sizeof(r));

    BLE_LOG("[BLE] clock synced: last refresh %lu, min gap %u s\n",
            (unsigned long)refreshGuardLastRefreshUtc(),
            (unsigned)refreshGuardMinGapSeconds());
}

// 用户接受了警告。武装一次性的授权，覆盖 App 即将重复的那条命令；授权在面板
// 真正被绘制时被消费掉，因此它不能被重放用于第二次刷新。
static void handleConfirmRefresh(uint8_t seq, const uint8_t *p, size_t len) {
    if (len < 1) { sendResponse(BLE_OP_CONFIRM_REFRESH, seq, BLE_ERR_LENGTH, nullptr, 0); return; }

    const uint8_t token = p[0];
    if (!refreshGuardArmGrant(token)) {
        // 未知、过期或已用过的令牌。App 应把它当作"再问用户一次"：重新发送那条
        // 被拒绝的命令以取得一个新令牌，而不是盲目重试这一个。
        sendResponse(BLE_OP_CONFIRM_REFRESH, seq, BLE_ERR_STATE, nullptr, 0);
        return;
    }
    sendResponse(BLE_OP_CONFIRM_REFRESH, seq, BLE_OK, &token, 1);
}

static void handleControlFrame(const uint8_t *f, size_t n) {
    if (n < BLE_FRAME_HEADER_LEN) return;   // 短到连帧头都读不出来

    // 整个请求使用同一把锁：一个控制帧会改动 loop 任务也会读取的若干字段
    // （会话、绘制标志、提示标志），而下面的处理函数可能嵌套进刷新守卫。加锁顺序
    // 永远是 状态 -> 守卫；没有任何地方按相反顺序获取它们。
    StateLock lock;

    const uint8_t opcode = f[0];
    const uint8_t seq = f[1];
    const uint8_t plen = f[3];

    // 先对 opcode 做范围检查，这样下面每个 sendResponse() 都保证构造出的 opcode
    // 落在响应区间（0x81..0xBF）内，而不会做出看起来像事件的东西。
    if (opcode == 0 || opcode > BLE_OP_MAX_REQUEST) {
        pushFrame(BLE_OP_ERROR, seq, BLE_ERR_OPCODE, &opcode, 1);
        return;
    }

    if ((size_t)plen + BLE_FRAME_HEADER_LEN > n) {
        sendResponse(opcode, seq, BLE_ERR_LENGTH, nullptr, 0);
        return;
    }
    const uint8_t *p = f + BLE_FRAME_HEADER_LEN;

    switch (opcode) {
        case BLE_OP_GET_INFO:
            handleGetInfo(seq);
            break;

        case BLE_OP_BEGIN_IMAGE:
            handleBeginImage(seq, p, plen);
            break;

        case BLE_OP_BLOCK_CRCS:
            handleBlockCrcs(seq, p, plen);
            break;

        case BLE_OP_RETRY_BLOCK:
            handleRetryBlock(seq, p, plen);
            break;

        case BLE_OP_SYNC_TIME:
            handleSyncTime(seq, p, plen);
            break;

        case BLE_OP_CONFIRM_REFRESH:
            handleConfirmRefresh(seq, p, plen);
            break;

        case BLE_OP_END_IMAGE:
            handleEndImage(seq, p, plen);
            break;

        case BLE_OP_ABORT:
            resetTransfer();
            s_renderPending = false;   // App 也要放弃这幅图像
            s_hintRequested = false;
            sendResponse(BLE_OP_ABORT, seq, BLE_OK, nullptr, 0);
            // 已经开始的一次绘制无法取消：面板正处于波形中途，停在那里会让它进入
            // 未定义状态，因此 DISPLAYED 事件仍然会跟着发出（防断开式绘制是刻意
            // 的设计）。把这一点说出来，才能让 App 不误以为屏幕不会变化。
            if (s_rendering) {
                static const char kCannotAbort[] = "render in progress, cannot be aborted";
                // 与 PING 同样的纪律：链路承载不了的帧会被协议栈截断，
                // 所以干脆不发送。
                if (frameFits(sizeof(kCannotAbort) - 1)) {
                    sendEvent(BLE_EVT_LOG, (const uint8_t *)kCannotAbort,
                              sizeof(kCannotAbort) - 1);
                }
                BLE_LOG("[BLE] ABORT while rendering: the refresh runs to completion\n");
            }
            sendStateEvent(BLE_STATE_CONNECTED, BLE_OK);
            BLE_LOG("[BLE] ABORT\n");
            break;

        case BLE_OP_SHOW_HINT:
            // 提示屏是对同一个帧缓冲的一次整屏刷新；在一次传输正写入其中、或一幅
            // 图像正被推送到面板时，无法绘制它。过去在这里返回 OK 意味着"接受了，
            // 然后被静默丢弃" - 现在 App 会知道原因。
            if (s_receiving || s_renderPending || s_rendering) {
                sendResponse(BLE_OP_SHOW_HINT, seq, BLE_ERR_BUSY, nullptr, 0);
                break;
            }
            // 绘制提示屏和其他任何整屏刷新一样，因此它也要遵守同样的面板冷却。
            // 一次确认授权只对它被签发时的那条命令有效。
            if (!refreshGuardMayRefreshFor(BLE_OP_SHOW_HINT)) {
                sendRefreshBlocked(BLE_OP_SHOW_HINT, seq);
                break;
            }
            s_hintRequested = true;
            sendResponse(BLE_OP_SHOW_HINT, seq, BLE_OK, nullptr, 0);
            break;

        case BLE_OP_PING:
            // 原样回显，但绝不发送链路承载不了的帧：长于 MTU-3 的 ATT 通知会被
            // 协议栈静默 TRUNCATED，交给 App 的帧其长度字节承诺的字节数会多于
            // 实际到达的字节数。这正是 ERR_MTU 存在的意义，而且文档本来就把
            // PING 的载荷上限设为 250 字节。
            if (!frameFits(plen)) {
                BLE_LOG("[PING] MTU %u cannot echo a %u-byte payload\n",
                        (unsigned)s_mtu, (unsigned)plen);
                sendResponse(BLE_OP_PING, seq, BLE_ERR_MTU, nullptr, 0);
                break;
            }
            sendResponse(BLE_OP_PING, seq, BLE_OK, p, plen);
            break;

        case BLE_OP_SET_OPTIONS: {
            if (plen < 1) { sendResponse(BLE_OP_SET_OPTIONS, seq, BLE_ERR_LENGTH, nullptr, 0); break; }
            const uint8_t opts = p[0];
            if (opts & (uint8_t)~BLE_OPT_VALID_MASK) {
                sendResponse(BLE_OP_SET_OPTIONS, seq, BLE_ERR_OPTIONS, nullptr, 0);
                break;
            }
            s_options = opts;
            const uint8_t echo = opts;
            sendResponse(BLE_OP_SET_OPTIONS, seq, BLE_OK, &echo, 1);
            BLE_LOG("[BLE] options=0x%02X\n", (unsigned)opts);
            break;
        }

        default:
            sendResponse(opcode, seq, BLE_ERR_OPCODE, nullptr, 0);
            break;
    }
}

// ---------------------------------------------------------------- 回调

class ControlCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c, ble_gap_conn_desc *desc) override {
        (void)desc;
        const NimBLEAttValue v = c->getValue();
        handleControlFrame(v.data(), v.length());
    }

    void onSubscribe(NimBLECharacteristic *c, ble_gap_conn_desc *desc, uint16_t subValue) override {
        (void)c;
        (void)desc;
        if (subValue == 0) return;   // 已取消订阅
        BLE_LOG("[BLE] CONTROL notifications enabled\n");
        sendStateEvent(s_receiving ? BLE_STATE_RECEIVING : BLE_STATE_CONNECTED, BLE_OK);
    }
};

class DataCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c, ble_gap_conn_desc *desc) override {
        (void)desc;
        const NimBLEAttValue v = c->getValue();
        handleData(v.data(), v.length());
    }
};

// 请求决定传输速率的链路参数。
//
// 若要让传输超过约 10 kB/s，这里没有任何一项是可选的：Android 对未提权外设的
// 默认连接间隔约为 48 ms，而且每个事件只发一次写入，这恰好就是实测到的
// 9.5 kB/s。两次调用都是请求 - 手机可能把它们钳制住 - 因此这里记录结果，
// 而 App 那边也被要求做好它的一半。
static void applyFastLinkParams(uint16_t connHandle) {
    // 两次调用都是 NimBLE 请求而不是命令：在 NimBLE-Arduino 1.4.3 里
    // updateConnParams() 和 setDataLen() 返回 void（手机可能静默地钳制或拒绝
    // 它们），所以这里唯一能检查的就是请求已经发出 - 真正的结果会体现在同一条
    // 日志行所报告的连接间隔和 MTU 上。
    if (s_server) {
        s_server->updateConnParams(connHandle,
                                   BLE_CONN_MIN_INTERVAL, BLE_CONN_MAX_INTERVAL,
                                   BLE_CONN_LATENCY, BLE_CONN_TIMEOUT);
        BLE_LOG("[BLE] conn params requested: %.2f-%.2f ms, latency %u, timeout %u ms\n",
                BLE_CONN_MIN_INTERVAL * 1.25, BLE_CONN_MAX_INTERVAL * 1.25,
                (unsigned)BLE_CONN_LATENCY, (unsigned)(BLE_CONN_TIMEOUT * 10));
    }

    // 数据长度扩展。没有它，控制器可能一直沿用 27 字节的默认 LL 载荷，把每次
    // 512 字节的 ATT 写入切成约 20 个 PDU。
    if (s_server) {
        s_server->setDataLen(connHandle, BLE_LL_TX_OCTETS);
        BLE_LOG("[BLE] data length requested: %u octets\n",
                (unsigned)BLE_LL_TX_OCTETS);
    }
}

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *server, ble_gap_conn_desc *desc) override {
        // 只允许一个中心设备。协议只有一个会话和一个帧缓冲；第二个对端可能向
        // 进行中的传输注入 DATA，或者读取/窃取状态事件。连接时广播会停止，但一条
        // 正确性规则不能依赖那个默认行为。
        if (s_connected) {
            BLE_LOG("[BLE] rejecting second central (handle %u)\n",
                    (unsigned)desc->conn_handle);
            server->disconnect(desc->conn_handle);
            return;
        }

        s_connected = true;
        s_everConnected = true;
        s_connCount++;
        s_options = 0;
        s_connHandle = desc->conn_handle;
        s_mtu = server->getPeerMTU(desc->conn_handle);

        BLE_LOG("[BLE] connected (#%lu, mtu=%u, interval %.2f ms)\n",
                (unsigned long)s_connCount, (unsigned)s_mtu,
                desc->conn_itvl * 1.25);

        applyFastLinkParams(desc->conn_handle);

        // App 应当会订阅；onSubscribe() 会发出第一个状态事件。这一个覆盖在
        // 订阅之前就写入的中心设备。
        sendStateEvent(BLE_STATE_CONNECTED, BLE_OK);
    }

    void onDisconnect(NimBLEServer *server, ble_gap_conn_desc *desc) override {
        // 被拒绝的第二个中心设备绝不能拆掉活动会话。
        if (desc && s_connHandle != desc->conn_handle) {
            BLE_LOG("[BLE] rejected second central disconnected\n");
            return;
        }

        {
            StateLock lock;
            s_connected = false;
            resetTransfer();
            s_hintRequested = false;

            // 刻意不清除 s_renderPending：一幅通过了所有 CRC 校验的图像在帧缓冲里
            // 已经是完整的，即使手机走开了也可以安全绘制。ABORT 才是取消它的
            // 显式方式。
        }

        BLE_LOG("[BLE] disconnected, advertising again\n");
        // advertiseOnDisconnect 已在 bleInit 中禁用，所以这里是唯一重新开始
        // 广播的地方（协议栈过去也会做这件事）。
        if (server) server->startAdvertising();
    }

    void onMTUChange(uint16_t mtu, ble_gap_conn_desc *desc) override {
        (void)desc;
        s_mtu = mtu;
        BLE_LOG("[BLE] MTU now %u -> chunk %u\n", (unsigned)mtu,
                (unsigned)dataChunkSize());
    }
};

// ---------------------------------------------------------------- 公开 API

bool bleInit(const char *deviceName, uint8_t *frameBuffer) {
    s_frame = frameBuffer;

    // 在任何回调可能触发之前创建。递归锁，因为处理函数会嵌套。
    if (!s_stateMux) s_stateMux = xSemaphoreCreateRecursiveMutex();

    strncpy(s_name, deviceName ? deviceName : "8adge", sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';

    NimBLEDevice::init(s_name);
    NimBLEDevice::setMTU(BLE_MTU_PREFERRED);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    s_server = NimBLEDevice::createServer();
    s_server->setCallbacks(new ServerCallbacks());
    // 广播由 onDisconnect() 重新开始；禁用协议栈自身的自动重启可以避免
    // 做两次。
    s_server->advertiseOnDisconnect(false);

    NimBLEService *svc = s_server->createService(BLE_SERVICE_UUID);

    s_ctrl = svc->createCharacteristic(
        BLE_CTRL_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
    s_ctrl->setCallbacks(new ControlCallbacks());

    // 不显式指定 max_len：NimBLECharacteristic 默认把它设为
    // BLE_ATT_ATTR_MAX_LEN（512），而主机拒绝任何长度 *大于* 该值的写入。
    // 正是这个上限让 BLE_DATA_CHUNK_MAX 取 512，而不是"MTU 允许多少就多少" -
    // MTU 为 517 时 ATT 载荷会是 514 字节，写入就会以 ATT 错误返回。
    s_data = svc->createCharacteristic(
        BLE_DATA_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    s_data->setCallbacks(new DataCallbacks());

    svc->start();

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();

    // 广播数据是手工拼的，而不是用
    // addServiceUUID()，因为 NimBLE 会先把服务 UUID 打包进去、名字排在后面：
    // 3（flags）+ 18（128 位 UUID）+ 16（名字）共 37 字节，于是名字会被挤进扫描
    // 响应，被动扫描只会看到一个匿名设备。名字在前才是 App 用来过滤的依据。
    //
    //   广播包             : flags + 完整本地名             (19 字节)
    //   扫描响应           : 128 位服务 UUID               (18 字节)
    //
    // Android 会把两者合并成一个 ScanRecord，因此针对服务 UUID 的 ScanFilter
    // 仍然能匹配，即使该 UUID 并不在广播包里。
    NimBLEAdvertisementData advData;
    advData.setFlags(0x02 /* LE General Discoverable，低功耗通用可发现 */);
    advData.setName(s_name);

    NimBLEAdvertisementData scanData;
    scanData.setCompleteServices(NimBLEUUID(BLE_SERVICE_UUID));

    adv->setAdvertisementData(advData);
    adv->setScanResponseData(scanData);
    adv->setScanResponse(true);
    adv->setMinInterval(0x20);   // 20 ms
    adv->setMaxInterval(0x40);   // 40 ms
    adv->start();

    BLE_LOG("[BLE] advertising as \"%s\"\n", s_name);
    return s_frame != nullptr;
}

const char *bleDeviceName() { return s_name; }

bool bleIsConnected() { return s_connected; }
bool bleHasEverConnected() { return s_everConnected; }
uint32_t bleConnectionCount() { return s_connCount; }
bool bleIsTransferring() { return s_receiving; }
bool bleIsFixingBlock() { return s_fixing; }

bool bleTakeRenderRequest(uint8_t *outFormat, uint32_t *outBytes) {
    StateLock lock;

    if (!s_renderPending) return false;
    if (!s_frame) {
        // 什么都无法绘制；丢弃这个请求总好过设置
        // s_rendering 并让它永远保持（那会让之后每个 BEGIN_IMAGE 都以 ERR_BUSY
        // 作答）。
        s_renderPending = false;
        return false;
    }
    if (outFormat) *outFormat = s_renderFormat;
    if (outBytes) *outBytes = s_renderBytes;
    s_renderPending = false;
    // 占用帧缓冲，直到调用方说绘制完成。没有这一点，在 15-25 s 的整屏刷新
    // 期间到达的 BEGIN_IMAGE 会改写正在被推送到面板的那些字节。
    s_rendering = true;
    return true;
}

void bleRenderFinished() {
    StateLock lock;
    // 当某个 App 的数据在绘制期间被丢弃时，只打一行、只打一次：没有它，
    // 唯一的痕迹就是一个时而收到 ERR_BUSY、时而收到 ERR_COOLDOWN 的
    // BEGIN_IMAGE，从中无法诊断出任何手机侧的 bug。s_dataHint 本身在这里刻意
    // 不清除 - 下一次 BEGIN_IMAGE 会清掉它，而在那之前，正是它告诉那些 END_IMAGE
    // 在绘制之后才到达的 App 为什么什么都没收到。
    if (s_dataHint == BLE_HINT_DATA_AFTER_END) {
        BLE_LOG("[BLE] DATA arrived while the panel was being painted and was dropped\n");
    }
    s_rendering = false;
}

bool bleBeginHintPaint() {
    StateLock lock;

    if (!s_frame) return false;
    if (s_receiving || s_renderPending || s_rendering) return false;
    s_rendering = true;
    return true;
}

void blePollTransferTimeout() {
    bool timedOut = false;
    uint32_t silenceMs = 0;
    uint32_t received = 0;
    uint32_t total = 0;

    {
        StateLock lock;

        if (!s_receiving) return;
        // BEGIN -> 第一个 DATA 之间合理地可能要等一会儿（量化、压缩）；
        // 之后的每一次间隔都算停滞。
        const uint32_t limit = s_firstByteSeen ? BLE_RX_TIMEOUT_MS : BLE_BEGIN_TIMEOUT_MS;
        silenceMs = (uint32_t)(millis() - s_lastRxMs);
        if (silenceMs < limit) return;

        // 否则，一个保持链路却停止发送的手机（App 被杀、一次从未重传的写入）
        // 会让会话卡在 RECEIVING/FIXING，并让之后每个 BEGIN_IMAGE 都得到
        // ERR_STATE。缓冲区里已经存有收到的内容；新的 BEGIN 会覆写它。
        received = s_received;
        total = s_total;
        resetTransfer();
        timedOut = true;
    }

    if (!timedOut) return;

    BLE_LOG("[BLE] transfer stalled for %lu ms (%lu/%lu bytes), resetting session\n",
            (unsigned long)silenceMs, (unsigned long)received, (unsigned long)total);
    // 在状态锁之外发送：notify() 会取 NimBLE 主机锁，把两者嵌套起来只会颠倒
    // 加锁顺序，没有任何好处。
    sendStateEvent(BLE_STATE_CONNECTED, BLE_OK);
}

bool bleTakeHintRequest() {
    if (!s_hintRequested) return false;
    s_hintRequested = false;
    return true;
}

uint8_t bleOptions() { return s_options; }

void bleNotifyState(uint8_t state, uint8_t error) { sendStateEvent(state, error); }
void bleNotifyRendering() { sendStateEvent(BLE_STATE_RENDERING, BLE_OK); }
void bleNotifyDisplayed(uint8_t error) {
    sendStateEvent(error == BLE_OK ? BLE_STATE_DISPLAYED : BLE_STATE_ERROR, error);
}

void bleNotifyLog(const char *text) {
    if (!text) return;
    sendEvent(BLE_EVT_LOG, (const uint8_t *)text, strlen(text));
}
