#ifndef BADGE8_BLE_PROTOCOL_H
#define BADGE8_BLE_PROTOCOL_H

// ============================================================================
//  8adge BLE 图片传输协议 - 线上（on-wire）常量。
//
//  协议 v3。完整规范 - 帧布局、状态机、
//  流控规则、重传握手、刷新冷却以及
//  Android 侧的检查清单 - 见 docs/BLE_PROTOCOL.md。本头文件是
//  数字的唯一真源；文档是散文描述。
//
//  请保持两者同步：这里某个常量变了，文档也要跟着改。
//
//  v2 相对 v1 的变化：
//    * 逐块 CRC32 与选择性重传（BLOCK_CRCS + RETRY_BLOCK），
//      因此受损区域只花一个块，而不是整张图；
//    * 窗口 ACK 变成纯粹的进度上报 - App 不再需要
//      在窗口边界停下（链路层本就有流控）；
//    * 连接时使用快速连接参数与数据长度扩展。
//
//  v3 相对 v2 的变化：
//    * 整屏刷新冷却，由固件强制执行，因为违反它会
//      不可逆地损伤面板。时钟由 App 提供
//      （SYNC_TIME），冷却期内请求的刷新会被拒绝并返回
//      ERR_COOLDOWN，直到用户确认（CONFIRM_REFRESH）；
//    * GET_INFO 报告冷却参数，让 App 在发送图片之前
//      就能给出警告。
// ============================================================================

#include <stdint.h>

// ---------------------------------------------------------------- GATT（通用属性配置文件）
// 两条特征值，都挂在一个 128 位的厂商服务下。每个 UUID 的前四个
// 字节以 ASCII 拼出 "BADG"，这样在
// BLE 扫描器里很容易认出来。
//
//   42414447-0001-4e5a-9c1a-6b7c8d9e0f11   服务
//   42414447-0002-4e5a-9c1a-6b7c8d9e0f11   CONTROL  write、write-nr、notify（可写、无响应写、通知）
//   42414447-0003-4e5a-9c1a-6b7c8d9e0f11   DATA     write、write-nr（可写、无响应写）
//
// CONTROL 承载两个方向的*全部*结构化流量：来自 App 的请求，
// 以及回给它（作为响应）加上未经请求的事件。DATA 只承载
// 图片字节，所以批量路径永远不会与控制帧共用同一个包队列 -
// 而且图片字节完全不带逐分片头部，这正是重点：按每次写入 512 字节算，
// 2 字节头部会占掉链路预算的 0.4%，用在块 CRC 本来就能
// 检测出来的东西上。
#define BLE_SERVICE_UUID "42414447-0001-4e5a-9c1a-6b7c8d9e0f11"
#define BLE_CTRL_UUID    "42414447-0002-4e5a-9c1a-6b7c8d9e0f11"
#define BLE_DATA_UUID    "42414447-0003-4e5a-9c1a-6b7c8d9e0f11"

// ---------------------------------------------------------------- 画布
// 手机与固件都工作在这个竖屏画布坐标系里。它*不是*
// 面板的原生尺寸：面板是 768x552 的横屏器件，设备自己把画布
// 旋转到上面（见 ble_geometry.h），所以 App 永远不需要
// 知道这块屏是朝哪边装的。
//
// 552 x 768 = 768 x 552 像素，所以旋转是精确的 - 像素数相同，
// 无缩放、无裁剪 - 下面两种格式的长度也与
// 它们在横屏下的长度相同。
#define BLE_IMAGE_WIDTH        552
#define BLE_IMAGE_HEIGHT       768
#define BLE_IMAGE_ROW_BYTES_2BPP (BLE_IMAGE_WIDTH / 4)   // 每行 138 字节
#define BLE_IMAGE_ROW_BYTES_1BPP (BLE_IMAGE_WIDTH / 8)   // 每行  69 字节

// ---------------------------------------------------------------- 分帧
// CONTROL 上的每一帧都以这个 4 字节头开始。载荷长度是
// 单字节，所以一帧最多 259 字节；控制流量从不会
// 接近这个上限。
//
//   [0] opcode      置位 bit7 = 设备 -> App（响应或事件）
//   [1] seq         从请求回显；未经请求的事件为 0
//   [2] status      结果码，仅响应中有（请求和事件中为 0）
//   [3] length      后面跟随的载荷字节数（0..255）
//   [4..] payload（载荷）
#define BLE_FRAME_HEADER_LEN  4
#define BLE_FRAME_MAX_PAYLOAD 255
#define BLE_FRAME_MAX_LEN     (BLE_FRAME_HEADER_LEN + BLE_FRAME_MAX_PAYLOAD)

// 所有多字节整数均为小端。
#define BLE_PROTOCOL_VERSION 3

// ---------------------------------------------------------------- 操作码
// App -> 设备（请求）。
enum : uint8_t {
    BLE_OP_GET_INFO    = 0x01,
    BLE_OP_BEGIN_IMAGE = 0x02,
    BLE_OP_END_IMAGE   = 0x03,
    BLE_OP_ABORT       = 0x04,
    BLE_OP_SHOW_HINT   = 0x05,
    BLE_OP_PING        = 0x06,
    BLE_OP_SET_OPTIONS = 0x07,
    // v2
    BLE_OP_BLOCK_CRCS  = 0x08,  // 声明每块的 CRC（在 BEGIN 之后）
    BLE_OP_RETRY_BLOCK = 0x09,  // 让设备准备好再次接收某一个块
    // v3
    BLE_OP_SYNC_TIME       = 0x0A,  // u64 UTC 秒：采纳 App 的时钟
    BLE_OP_CONFIRM_REFRESH = 0x0B,  // u8 token：用户批准了一次被拦下的刷新
};

// 设备 -> App 的响应：请求操作码 | 0x80，即请求范围 0x01..0x0B
// 对应 0x81..0x8B。
#define BLE_OP_RESPONSE(op) ((uint8_t)((op) | 0x80))

// 仍然属于请求的最大操作码。比它更大的都是保留值，
// 收到这类帧会以 BLE_OP_ERROR 应答。
#define BLE_OP_MAX_REQUEST 0x3F

// 唯一一个用于“请求无法以 `opcode | 0x80` 应答”场景的操作码，
// 因为请求操作码本身超出范围（0 或 > 0x3F）。回显它
// 会落到事件范围内，所以违规的操作码改为放在载荷里：
// `u8 offending_opcode`。
#define BLE_OP_ERROR 0xE0

// 设备 -> App 的未经请求的事件。
//
// 它们从 0xC0 开始是刻意的：响应的范围是 0x81..0xBF，这样事件
// 永远不会被误认为响应。
enum : uint8_t {
    BLE_EVT_STATE      = 0xC0,  // u8 state, u8 error
    BLE_EVT_PROGRESS   = 0xC1,  // u32 received, u32 total, u8 session, u8 flags
    BLE_EVT_LOG        = 0xC2,  // UTF-8 文本，无 NUL 结尾
        // v3：刷新因冷却被拒绝。App 必须询问
        // 用户，若用户同意，则在 BLE_OP_CONFIRM_REFRESH 中交回 token，
        // 随后重发它最初发出的同一条命令。
        //   u8  token            用于关联这次确认
    //   u8  reason           BLE_GUARD_REASON_*
        //   u8  for_opcode       被拒绝的请求，方便 App 重试
    //   u8  reserved         0
        //   u16 wait_seconds     冷却自行结束前还需等待的秒数
        //   u16 min_gap_seconds  设备配置的最小值
        //   u32 elapsed_seconds  距上次刷新过去了多久（当 reason ==
        //                        BLE_GUARD_REASON_CLOCK_UNKNOWN 时是下界）
    BLE_EVT_REFRESH_BLOCKED = 0xC3,
};

// 操作码 0xC1 在 v1 里叫 EVT_WINDOW_ACK，当时它是一个停等
// 信用额度。现在它只是纯粹的进度上报，App 完全可以忽略它、
// 不据此调节节奏；改名是为了避免有人重新实现旧的屏障语义。

// EVT_PROGRESS 的 flags
#define BLE_PROGRESS_FLAG_BLOCK_DONE 0x01  // 刚刚完成了一整个块的校验

// ---------------------------------------------------------------- 数据提示
// ERR_STATE 的 END_IMAGE 应答所携带的那一个字节（docs §5.4）：DATA
// 到达时没有会话可以接收它。设备会继续接受并丢弃这些写入，而进度上报
// 只在每个 BLE_DATA_CHUNK_MAX 窗口才发一次，所以一个把数据一路流水线
// 发进刚完成的图片刷屏过程中的 App，可能一直在等一个永远不会到来的
// DISPLAYED。这个字节说明原因。App 必须忽略未知值。
#define BLE_HINT_NONE              0x00  // 无内容可报告
#define BLE_HINT_DATA_AFTER_END    0x01  // 传输已结束，这些数据无处可去
#define BLE_HINT_DATA_IGNORED      0x02  // 超出 total_len 的字节被丢弃

// ---------------------------------------------------------------- 状态
// 响应中 STATUS 字节的结果码。BLE_OK 是唯一的成功值。
enum : uint8_t {
    BLE_OK           = 0x00,
    BLE_ERR_OPCODE   = 0x01,  // 未知操作码
    BLE_ERR_LENGTH   = 0x02,  // 帧太短，或载荷长度不匹配
    BLE_ERR_STATE    = 0x03,  // 当前状态下不允许该命令
    BLE_ERR_FORMAT   = 0x04,  // 不支持的图片格式
    BLE_ERR_SIZE     = 0x05,  // total_len / block_size / count 不匹配
    BLE_ERR_CRC      = 0x06,  // 整图 CRC 不符，需从 BEGIN 重来
    BLE_ERR_MEM      = 0x07,  // 设备无法分配帧缓冲
    BLE_ERR_PANEL    = 0x08,  // 面板刷新失败（BUSY 超时）
    BLE_ERR_BUSY     = 0x09,  // 设备仍在绘制上一张图片
    BLE_ERR_OVERFLOW = 0x0A,  // 数据多于 BEGIN_IMAGE 中声明的量
    BLE_ERR_OPTIONS  = 0x0B,  // 不支持的选项位
    BLE_ERR_BLOCKS   = 0x0C,  // v2：部分块失败 - 重传它们，见位图
    BLE_ERR_COOLDOWN = 0x0D,  // v3：被拒绝，距上次整屏刷新太近
    BLE_ERR_MTU      = 0x0E,  // v3.1：该应答装不进协商的 MTU
};

// 上面的状态码由响应和 EVT_STATE.error 共用。
//
// ERR_MTU 表示该请求无法在不把应答按协商的 ATT MTU 截断的情况下
// 应答。App 必须请求更大的 MTU（517 是首选值）后重试。
// 它只会在 GET_INFO 以及失败位图装不下的 BEGIN_IMAGE 上返回；
// 没有任何状态被改变。

// ---------------------------------------------------------------- 状态
// EVT_STATE 第一个载荷字节的取值。
enum : uint8_t {
    BLE_STATE_IDLE        = 0,
    BLE_STATE_ADVERTISING = 1,
    BLE_STATE_CONNECTED   = 2,
    BLE_STATE_RECEIVING   = 3,
    BLE_STATE_VERIFYING   = 4,
    BLE_STATE_RENDERING   = 5,
    BLE_STATE_DISPLAYED   = 6,
    BLE_STATE_HINT_SHOWN  = 7,
    BLE_STATE_ERROR       = 8,
    BLE_STATE_FIXING      = 9,  // v2：正在接收一个重传的块
};

// ---------------------------------------------------------------- 格式
// BEGIN_IMAGE 接受的图片载荷格式。行宽为 BLE_IMAGE_WIDTH = 552
// 像素，每字节打包 4 个（2bpp）或 8 个（1bpp）像素，MSB 在前，自上
// 而下，行间无填充。
//
// 长度与横屏 768x552 载荷相同，因为
// 旋转纯粹是像素置换 - 见 ble_geometry.h。
enum : uint8_t {
    // 2bpp，面板的原生打包方式：每字节 4 个像素，最高有效位那一对
    // 最靠左，行主序，每行 138 字节，共 768 行。总计 105984 字节。
    // 像素取值：0 = 黑，1 = 白，2 = 黄，3 = 红。
    BLE_FMT_2BPP = 1,

    // 1bpp 单色：1 = 白，0 = 黑，最高有效位最靠左，
    // 行主序，每行 69 字节，共 768 行。总计 52992 字节。设备在
    // 接收时把它转成 2bpp，所以只有黑白的图片用一半的时间就能传完，
    // App 也不需要四色量化器。
    BLE_FMT_1BPP = 2,
};

// ---------------------------------------------------------------- 选项
// SET_OPTIONS 的位图。每次新连接都重置为 0。
#define BLE_OPT_DISABLE_HINT 0x01  // 永不绘制 2 分钟的提示屏
#define BLE_OPT_VALID_MASK   0x01

// ---------------------------------------------------------------- 刷新守卫
// 两次整屏刷新之间的最短秒数。固件强制执行它，
// 因为四色墨水屏刷新 - 一段很长的双极性交流波形 - 若在颜料颗粒
// 稳定之前再次启动，会不可逆地损伤面板。机制见
// src/refresh_guard.{h,cpp}，App 侧的握手见文档。
//
// App 不得把它写死：它由 GET_INFO 以及每次
// EVT_REFRESH_BLOCKED 报告。它也是 App 判断能否
// 干脆跳过询问用户所需的唯一数值。
#define BLE_REFRESH_MIN_GAP_S 180

// 刷新被拒绝的原因，位于 EVT_REFRESH_BLOCKED 的 reason 字节。它告诉
// App 应该多信任同一事件中的 `elapsed_seconds`。
//
// 设备本身没有时钟 - 它从不连接任何东西，其 RTC 也无法熬过断电 -
// 所以它从 App 取时间（BLE_OP_SYNC_TIME），否则退回到由自身
// 运行时长推导出的保守边界。
//
//   CLOCK_UNKNOWN  没有时钟，或最新一次刷新无法定日期。所报告的
//                  已过时间是*下界*：真实间隔至少那么大，也可能大得多。
//                  这并不是出错，只是设备无法更精确。对话框措辞
//                  应与此相符（"至少 ..."）。
//   CLOCK_KNOWN    App 的时钟正在使用，间隔是精确的。
//   CLOCK_BEHIND   App 的时钟看起来早于所记录的刷新时间 -
//                  有人发了毫秒而不是秒，或者手机时钟向后走了。
//                  绝不能当作时间已经过去；最安全的解读是
//                  “间隔为零”。
//
// 这些编码刻意落在 0x00-0x3F 范围内：它们在协议中走
// 单字节字段，而高位留给帧方向。
enum : uint8_t {
    BLE_GUARD_REASON_CLOCK_UNKNOWN = 0,
    BLE_GUARD_REASON_CLOCK_KNOWN   = 1,
    BLE_GUARD_REASON_CLOCK_BEHIND  = 2,
};

// ---------------------------------------------------------------- 辅助函数
static inline uint16_t bleRd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t bleRd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void bleWr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static inline void bleWr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

#endif // BADGE8_BLE_PROTOCOL_H
