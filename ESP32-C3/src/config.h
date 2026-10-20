#ifndef BADGE8_BLE_CONFIG_H
#define BADGE8_BLE_CONFIG_H

// ============================================================================
//  8adge BLE 固件 - 板级与构建常量
//
//  只有一种板子：ESP32-C3-SuperMini + 3.98" 四色墨水屏（A1 面板，
//  768x552，2bpp）。本构建没有 WiFi、没有强制门户、没有 OTA，也没有
//  服务器下发的内容 - 手机只通过蓝牙 LE 与面板通信，别无他途。
//  见 docs/BLE_PROTOCOL.md。
//
//  唯一写入 flash 的内容是一条 16 字节记录：面板上次刷新的时间，
//  这样 180 s 的防残影冷却就能跨断电存活。见 src/refresh_guard.{h,cpp}。
// ============================================================================

#include <Arduino.h>

// ---------------------------------------------------------------- 面板引脚
// 软件模拟（bit-bang）的 4 线 SPI，厂商接线方式。
#define PIN_EPD_MOSI   6
#define PIN_EPD_SCK    4
#define PIN_EPD_CS     7
#define PIN_EPD_DC     1
#define PIN_EPD_RST    2
#define PIN_EPD_BUSY   10

// ---------------------------------------------------------------- 按键
// GPIO9 是 BOOT 按键，同时也是 ESP32-C3 的 strapping 引脚，以
// INPUT_PULLUP 读取（按下 = LOW）。只用于两项便利功能，两者都不是
// BLE 功能正常工作的必要条件：
//   短按                     重绘“请通过蓝牙连接”提示屏
//   按住 CFG_BTN_REBOOT_MS   重启设备
#define PIN_CFG_BTN        9
#define CFG_BTN_REBOOT_MS  8000UL
#define BTN_DEBOUNCE_MS    40UL

// ---------------------------------------------------------------- 状态 LED
// C3-SuperMini 的板载蓝色 LED 接在 GPIO8 上，且为低电平有效。
// GPIO8 是复位时被采样的 strapping 引脚，所以 ledInit() 让它停在 OFF。
#define PIN_LED            8
#define PIN_LED_ACTIVE_LOW 1

// ---------------------------------------------------------------- 引脚自检
// 放在编译期，因为这里打错的数字否则要到面板一直空白或 LED 永远不亮时才
// 暴露。GPIO8（LED）和 GPIO9（BOOT）是这块板子的 strapping 引脚，面板不使用
// 它们；GPIO18/19 是 C3 的 USB D-/D+，本构建的 CDC 控制台需要它们。
constexpr bool pinIn(const int *p, int n, int pin) {
    return n <= 0 ? false : (p[0] == pin || pinIn(p + 1, n - 1, pin));
}

constexpr bool pinsUnique(const int *p, int n) {
    return n <= 1 ? true : (!pinIn(p + 1, n - 1, p[0]) && pinsUnique(p + 1, n - 1));
}

constexpr int kUsedPins[] = {
    PIN_EPD_MOSI, PIN_EPD_SCK, PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY,
    PIN_CFG_BTN, PIN_LED,
};
static_assert(pinsUnique(kUsedPins, 8), "two of the board's functions share a pin");
static_assert(!pinIn(kUsedPins, 6, 8) && !pinIn(kUsedPins, 6, 9),
              "the panel must not claim a strapping pin (GPIO8 LED / GPIO9 BOOT)");
static_assert(!pinIn(kUsedPins, 8, 18) && !pinIn(kUsedPins, 8, 19),
              "GPIO18/19 are USB D-/D+ and must stay free for the console");

// ---------------------------------------------------------------- 几何
// 面板本身是 768x552 的横屏器件，2bpp。
//
// 这些数字只存在一处，位于 epd_398_4color.h（EPD398_WIDTH/HEIGHT/BPP），
// 驱动和 ble_geometry.h 也是针对它编译的。这里曾经有第二份副本
//（EPD_WIDTH/EPD_HEIGHT/EPD_BPP），platformio.ini 里还有作为 -D 标志的第三份；
// 这些副本之间只跟彼此不一致，而这正是那种最终只以屏上图片损坏形式暴露的漂移。
// 画布与面板的关系现在由 ble_geometry.h 在编译期检查。

// ---------------------------------------------------------------- 方向
// 手机发送的一切、以及本固件绘制的一切，都用 552x768 的竖屏*画布*表示，
// 设备在最后一步把它旋转到横屏面板上（见 ble_geometry.h）。竖屏相框正是
// 本构建的目的，所以画布是 App 天然的工作空间：
// 它自上而下地打包图片，与屏幕上的样子完全一致。
//
// 这是画布送往面板时施加的顺时针旋转：
//
//   90   画布左上角落在面板的右上角   （默认）
//   270  画布左上角落在面板的左下角   （比 90 再转 180 度）
//
// 180 是有意不提供的：转 180 度后位图仍是 552x768，无法覆盖到 768x552 的面板上。
// 在这块屏上竖屏永远是 ±90，上面两个值就是“正向或倒置”这一对。
//
// 如果刷好的设备旋转方向不对 - 因为它的面板在相框里是反着装的 - 把这里
// 在 90 和 270 之间切换再重新刷写。提示屏和手机图片都跟随它，所以它们
// 永远不会互相矛盾。
//
//（若最终还是要做横屏相框，需要把 ble_protocol.h 里的 BLE_IMAGE_WIDTH/HEIGHT
// 也改成 768x552，并重新构建 App - 手机打包依据的是画布尺寸。）
#define BLE_ROTATION 90

// ---------------------------------------------------------------- 提示屏
// 空闲屏是一个四色边框，说明文字放在中间。画布尺寸为 552x768。
//
//   HINT_RING_THICKNESS  边框色带宽度，单位像素
//   HINT_RING_BLOCKS     色环被均分成多少个色块（绕整整一圈）。颜色按
//                        黑 -> 白 -> 红 -> 黄 循环，所以取 4 的倍数能让
//                        循环干净收尾；24 块即 6 个完整循环。
//   HINT_RING_KEYLINE    紧贴色环内侧画的一条细黑线。没有它，色环里的
//                        白色块与白色内部无法区分，边框看起来就像断了。
//                        设为 0 即可去掉它。
#define HINT_RING_THICKNESS 24
#define HINT_RING_BLOCKS    24
#define HINT_RING_KEYLINE   3

// ---------------------------------------------------------------- 蓝牙（BLE）
// 广播名称为 "8adge-" 加上 WiFi station MAC 的最后 5 位十六进制数字，
// 顺序就是 esp_read_mac() 填充它们的顺序。该后缀对每块板子是稳定的，所以
// 见过该设备的手机能再次认出同一个名字；App 不得解析它，GET_INFO 会报告
// 身份字段。
#define BLE_NAME_PREFIX   "8adge-"
#define BLE_NAME_SUFFIX_LEN 5

// 首选的 ATT MTU。517 是 Android 中心设备会协商到的最大值；
// 每次写入的可用载荷是 MTU-3。更小的中心设备自然只会得到更小的分片 -
// 协议会在 GET_INFO / READY 中报告真实值。
#define BLE_MTU_PREFERRED 517

// 固件准备接受的最大 DATA 写入，无论 MTU 是多少。
//
// 512 不是调优选择，而是 NimBLE 的 BLE_ATT_ATTR_MAX_LEN：特征值的 value
// 缓冲区默认为该大小，主机拒绝任何比它更长的写入。MTU 为 517 时 ATT 载荷
// 会是 514 字节，那样只会以 ATT 错误返回。
#define BLE_DATA_CHUNK_MAX 512

// ---------------------------------------------------------------- 吞吐
//
// 在真实硬件上用默认设置测得：约 9.5 kB/s，即连接间隔约 48 ms 时每个连接
// 事件大约一次 512 字节写入（连接未被提升优先级时 Android 的默认值）。
// 传输本身并不是瓶颈 - 链路大部分时间都是空闲的。三件事可以解决它，而
// 只有前两件是固件的职责：
//
//  1. 请求较短的连接间隔 + 零延迟（见下）。在 15 ms 下，同样的每事件一次
//     写入可得到约 34 kB/s；手机可能会限制该请求。
//  2. 请求数据长度扩展（见下），这样一次 ATT 写入不会被切
//     成二十个 27 字节的链路层 PDU。
//  3. App 必须调用 requestConnectionPriority(CONNECTION_PRIORITY_HIGH)，并把
//     写入排成流水线，而不是每传一个分片就等一次回调。
//     见 docs/BLE_PROTOCOL.md - 固件无法替它完成。
//
// 连接间隔以 1.25 ms 为单位：6 = 7.5 ms，12 = 15 ms。
// Android 可能批准比请求更宽的间隔；7.5-15 ms 正是它为自己“高优先级”
// 连接采用的区间，所以它通常会同意。
#define BLE_CONN_MIN_INTERVAL 6
#define BLE_CONN_MAX_INTERVAL 12
// 连接延迟，以事件为单位（0 = 设备在每个事件中都应答）。
#define BLE_CONN_LATENCY 0
// 监督超时，以 10 ms 为单位。必须满足
// timeout*10 > (1+latency)*maxInterval*1.25*2，即此处 > 30 ms；2 s
// 远高于此，同时仍能从手机走出通信距离的情况中恢复。
#define BLE_CONN_TIMEOUT 200

// 数据长度扩展：我们请求的最大链路层载荷。251 是 BLE 的最大值（4.2 的特性）。
// 没有它，控制器可能停留在 27 字节的默认值，而每次 512 字节的 ATT 写入
// 就要花掉约 20 个 LL 包。
#define BLE_LL_TX_OCTETS 251

// 上面的数字之间存在若干由规范（以及 NimBLE）在运行时强制的关系，过去只写在
// 文字里。在这里检查它们很便宜，而搞错其中一个会表现为传输中途断链，或者写入
// 被主机静默拒绝。
//   * 监督超时（10 ms 单位）必须大于
//     (1 + latency) * maxInterval * 1.25 ms * 2  -> 两边乘以 20 以保持
//     整数：timeout*20 > (1+latency)*maxInterval*5
//   * DATA 分片上限就是 NimBLE 的特征值 value 缓冲区
//     （BLE_ATT_ATTR_MAX_LEN = 512）：更长的写入会以 ATT 错误返回
static_assert((int)BLE_CONN_TIMEOUT * 20 > (1 + (int)BLE_CONN_LATENCY) * (int)BLE_CONN_MAX_INTERVAL * 5,
              "BLE_CONN_TIMEOUT is too short for the requested interval/latency");
static_assert(BLE_DATA_CHUNK_MAX <= 512,
              "BLE_DATA_CHUNK_MAX exceeds NimBLE's 512-byte characteristic value buffer");
static_assert(BLE_CONN_MIN_INTERVAL <= BLE_CONN_MAX_INTERVAL,
              "the connection interval range is inverted");

// 设备在多少个 DATA 分片之后上报一次进度。这是*进度上报*，不是
// 停等屏障：ACK 尚在途中时 App 仍可继续发送。链路层本身已经保证投递与
// 流控（每个 LL 包都被确认或重传，控制器也不会接受超出其缓冲能力的数据），
// 所以等待这些 ACK 的 App 只是在无谓地拖慢自己。32 个分片 = 16 KB = 每张图
// 约 6 次上报。
#define BLE_WINDOW_CHUNKS 32

// 设备等待下一个 DATA 字节（或 RETRY_BLOCK / BLOCK_CRCS 帧）多久之后
// 放弃会话并回到 CONNECTED。
// 按实测的 9.5 kB/s，一张完整的 2bpp 图片需要约 11 s，而两次 512 字节写入
// 之间的间隔只有几十毫秒，所以 15 s 的静默只可能意味着 App 卡死了。
// 没有这条，一个被杀掉却留着链路的 App 会把设备卡在 RECEIVING 状态，并在
// 断开之前用 ERR_STATE 拒绝之后每一次 BEGIN_IMAGE。
#define BLE_RX_TIMEOUT_MS 15000UL

// 接收路径上两条高频日志行之间的最短间隔
//（重复/丢弃的分片、反复 CRC 失败的块）。ARDUINO_USB_CDC_ON_BOOT=1 时
// Serial.printf 运行在 NimBLE host 任务里，而 USB CDC 的写路径在主机已连接
// 但不读取时可能阻塞，所以一个繁忙或行为异常的 App 绝不能把控制台变成卡顿源。
#define BLE_LOG_THROTTLE_MS 300UL

// BEGIN_IMAGE 与第一个 DATA 字节之间的宽限期。App 在宣布传输之后可能还需要
// 一段时间来量化/压缩图片；15 s 的数据超时不应在第一个字节还没发出时
// 就到期。
#define BLE_BEGIN_TIMEOUT_MS 60000UL

// App 提供的时钟不可信。在一次上电周期内，被采纳的时间最多只能比设备自身的
// 运行时长快出这个容差，所以重复的 SYNC_TIME 帧无法把冷却“快进”。
//（上电后的第一次同步完全无法校验 - 这块板子上没有可信的时间源 -
// 并已作为信任边界记录在案。）
#define BLE_CLOCK_MAX_FORWARD_SKEW_S 30

// ---------------------------------------------------------------- 完整性
// 图片校验按*块*进行，而不是整张图一次完成，因此损坏或丢失的区域只需重传
// 一个块，而不是整张 106 KB。块大小是 1 << BLE_BLOCK_SHIFT；12 得到 4 KB，
// 即一整帧 2bpp 图有 26 块。
//
// App 预先声明每个块的 CRC（BLOCK_CRCS），END_IMAGE
// 则报告校验失败的块的位图。
#define BLE_BLOCK_SHIFT_DEFAULT 12     // 4096 字节的块
#define BLE_BLOCK_SHIFT_MIN 11         // 2 KB
#define BLE_BLOCK_SHIFT_MAX 14         // 16 KB
// 块数量的上限，也就是 END_IMAGE 响应能携带的失败位图大小：
// 128 块 = 16 字节。105984 / 1024 = 104，所以
// 即使 1 KB 的块也能放下。
#define BLE_MAX_BLOCKS 128

// ---------------------------------------------------------------- 刷新冷却
// 两次整屏刷新之间的最短间隔。这不是讲究，而是面板寿命限制：四色刷新会驱动
// 15-25 s 的双极性交流波形，物理地把红/黄/黑/白颜料颗粒推过微胶囊流体；
// 彩色颜料迁移率低，在前一次尚未稳定时就启动下一次刷新，会让极性相反的
// 颗粒聚集在一起。这种损伤 - 串色、发红的白、灰色残影 - 是不可逆的，
// 无论之后再做多少次“刷新成白色”都无济于事。厂商对四色屏规定 >= 180 s，
// 而且多数直接在控制器里禁用了局部刷新；本固件同样没有局部刷新路径。
//
// 由 src/refresh_guard.{h,cpp} 强制执行，它也解释了在没有自身时钟的设备上
// 如何测量该间隔。App 可以请求覆盖它：用户看到警告并确认后，被拦下的那一次刷新
// 一定放行（v1.3.2 起没有额外的时间下限），代价是每确认一次就多刷一次。
// 见 docs/BLE_PROTOCOL.md。
#define BLE_REFRESH_MIN_GAP_MS 180000UL   // 180 s，厂商规定的最小值

// 用户确认的有效期。它必须长于一次图片传输（链路好时几秒，链路差时更久），
// 但也不能久留，否则用户为一次刷新给出的确认会悄悄授权
// 下一次刷新。
#define BLE_REFRESH_GRANT_MS 120000UL

// GET_INFO 应答中报告的固件版本。请与文档保持同步。
// 1.1.0：协议 v2 - 逐块 CRC 与选择性重传、快速连接参数、
// 数据长度扩展、非阻塞的进度上报。
// 1.2.0：协议 v3 - 整屏刷新冷却（SYNC_TIME / CONFIRM_REFRESH /
// EVT_REFRESH_BLOCKED），以及本构建唯一写入 flash 的内容。
// 1.2.1：修复 1bpp 的 END_IMAGE CRC（格式选择器曾存放在 bool 里，导致
// BLE_FMT_1BPP 退化成 BLE_FMT_2BPP），并加入传输停滞超时。
// 1.3.0：审计修复 - 提示屏绘制占用帧缓冲、跨任务临界区、不可信时钟钳制
// 与强制刷新下限（已在 1.3.2 中移除）、MTU 校验、BEGIN/END 字段校验、保守的 NVS 记录、
// 抗回绕的 LED 保持。
// 1.3.1：第二次审计 - END_IMAGE 成功应答恢复为文档规定的 8 字节
//（之前发 26 字节，其中 18 字节是未初始化的栈）；超长 DATA 改为忽略
// 而不是让一张已校验通过的图作废；无会话时的 END_IMAGE 回
// ERR_STATE + 一个 data_hint 字节；END_IMAGE 失败时补发 RECEIVING；PING
// 回显受 MTU 门控；RETRY_BLOCK 校验其保留字段；有时钟时
// 确认下限按真实间隔衡量（该下限已在 1.3.2 中移除）。
// 1.3.2：取消 CONFIRM_REFRESH 的时间下限 —— 用户确认后一定放行，不再因
// “距上次刷新不足 60 秒 / 上次是强刷时不足 180 秒”回 ERR_STATE。旧规则让
// “仍然刷新”按钮看起来完全无效（设备拒绝、屏幕不变，App 也无从解释）。
// 令牌的约束不变：只为被冷却拒过的请求签发，并在一次绘制后被消耗。
#define FW_VERSION_MAJOR 1
#define FW_VERSION_MINOR 3
#define FW_VERSION_PATCH 2

// ---------------------------------------------------------------- 行为
// 上电时不去动面板（电子墨水会保留上一张图）。如果到这个时间为止
// 还没有中心设备连接，就改为绘制“请通过蓝牙连接”提示屏。一旦图片确实
// 传输完成，本次上电周期内就不会再画提示屏，所以一幅已经完成的图片不会
// 在两分钟后被覆盖。
#define BLE_HINT_DELAY_MS (2UL * 60UL * 1000UL)

// 串口控制台。设为 0 可把日志编译掉。
#ifndef BLE_DEBUG
#define BLE_DEBUG 1
#endif

#if BLE_DEBUG
#define BLE_LOG(...) Serial.printf(__VA_ARGS__)
#else
#define BLE_LOG(...) do {} while (0)
#endif

#endif // BADGE8_BLE_CONFIG_H
