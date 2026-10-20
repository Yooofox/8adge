// ============================================================================
//  8adge BLE 固件 - ESP32-C3-SuperMini + 3.98" 四色墨水屏。
//
//  本构建完全没有 WiFi 协议栈。没有强制门户、没有 HTTP
//  客户端、没有 OTA、没有服务器下发的内容，也没有任何网络代码 -
//  唯一的入口是蓝牙 LE，手机唯一能做的事就是发送
//  一张图片。线格式见 docs/BLE_PROTOCOL.md。
//
//  行为：
//
//    1. 上电后广播 "8adge-XXXXX"（station MAC 的最后 5 位十六进制
//       数字），并且不碰面板 - 电子墨水会保留屏上
//       原有内容。
//    2. 如果 BLE_HINT_DELAY_MS（2 分钟）内没有任何设备连接，就用
//       设备名和“请使用蓝牙连接：”重绘面板。
//    3. 一次校验通过的图片传输会用该图片重绘面板。之后在本次上电周期
//       内自动空闲提示被抑制，但显式请求（BOOT 短按，或 App 的
//       SHOW_HINT）仍可在其上重绘提示屏 - 那始终是用户/App 发起的。
//    4. 上述每一次重绘都要向面板偿还一次冷却：四色
//       刷新会驱动 15-25 s 的交流波形，过早重复会永久
//       损伤面板。src/refresh_guard.{h,cpp} 负责该规则；本
//       文件只在绘制前向它询问，绘制后回报。在没有
//       时钟时（还没人连接过），一次上电周期里的第一次提示屏因此
//       可能晚到多达 BLE_REFRESH_MIN_GAP_MS 的运行时长，而正是这一点
//       让该规则可被证明，而不只是大概率成立。
//    5. 两条绘制路径都会在绘制前占用那唯一的帧缓冲：提示屏绝不能
//       与手机向同一段字节流式传图相互竞争。
//
//  一切都由 BLE 回调事件驱动，所以 loop() 是一个很薄的
//  轮询器：没有状态机，除了那一个空闲截止时间外没有定时器。
// ============================================================================

#include <Arduino.h>
#include <string.h>

#include "esp_mac.h"
#include "esp_system.h"

#include "ble_protocol.h"
#include "ble_screen.h"
#include "ble_service.h"
#include "config.h"
#include "epd_398_4color.h"
#include "led.h"
#include "refresh_guard.h"

// ---------------------------------------------------------------- 状态

static uint8_t *s_frame = nullptr;       // 那唯一的 105984 字节缓冲区
static char s_deviceName[32] = {0};

static bool s_gpioReady = false;         // epd398GpioInit() 在首次使用时运行
static bool s_imageShown = false;        // 来自手机的图片已在屏上
static bool s_hintShown = false;         // 自动提示屏已经画过
static bool s_hintWanted = false;        // 有人请求了提示屏，但面板
static const char *s_hintWhy = nullptr;  //   仍处于冷却期内
static uint8_t s_hintOpcode = 0;         // 被拒绝的命令，所以只有该
                                         //   命令的授权才能放行它
static bool s_hintAuto = false;          // 待处理的提示屏来自空闲
                                         //   截止时间，即它请求的是“没有
                                         //   设备连接”这一会过期的前提
static unsigned long s_idleSince = 0;    // 链路上次进入空闲的时间
static bool s_wasConnected = false;
static bool s_errorHold = false;         // LED 错误状态的保持
static unsigned long s_errorSince = 0;

static unsigned long s_btnDownAt = 0;
static bool s_btnDown = false;
static bool s_btnFired = false;

// ---------------------------------------------------------------- 辅助函数

// "8adge-" + station MAC 的最后 5 位十六进制数字，所以名字对每块板子
// 都是稳定的，App 每次看到的都是同一台设备。
//
// 这里的字节序很关键，而且很容易搞错：
//   * esp_read_mac() 以大端填充数组 - mac[0] 是第一个八位组，
//     也就是 MAC 通常的打印顺序；
//   * ESP.getEfuseMac() 返回的 uint64_t 的*最低*字节才是第一个
//     八位组，所以必须按最低有效字节优先读出。
// 把两者搞混不会大声报错：它会得到一个字节序颠倒的地址，
// 而它看起来依然像是完全合理的名字 - MAC 90:DA:72:88:D1:34 会变成
// "8adge-2DA90" 而不是 "8adge-8D134"。
static void buildDeviceName() {
    uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
    bool fromEfuse = false;

    // 注意：ESP_MAC_WIFI_STA 是枚举值，不是宏。用
    // `#if defined(ESP_MAC_WIFI_STA)` 测试它永远是假，会静默地把
    // esp_read_mac() 这条路径编译掉，而上面那个颠倒的名字正是这样产生的。
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        fromEfuse = true;
        const uint64_t efuse = ESP.getEfuseMac();
        for (int i = 0; i < 6; i++) {
            mac[i] = (uint8_t)((efuse >> (8 * i)) & 0xFF);   // 低字节优先
        }
    }

    char hex[13];
    snprintf(hex, sizeof(hex), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    const int len = (int)strlen(hex);
    const char *suffix = (len >= BLE_NAME_SUFFIX_LEN) ? hex + (len - BLE_NAME_SUFFIX_LEN) : hex;

    snprintf(s_deviceName, sizeof(s_deviceName), "%s%s", BLE_NAME_PREFIX, suffix);
    BLE_LOG("[MAIN] device name %s (mac %s%s)\n", s_deviceName, hex,
            fromEfuse ? ", read from eFuse" : "");
}

// 面板引脚在首次使用时才配置，绝不在启动时配置：把 RST 配成
// 输出原则上无害，但“上电不得打扰面板”这件事，靠直到即将
// 绘制之前都不碰它，更容易保证。
static void ensurePanelGpio() {
    if (s_gpioReady) return;
    epd398GpioInit();
    s_gpioReady = true;
}

static bool paintHint(const char *why, uint8_t opcode) {
    if (!s_frame) return false;

    // 在整个 15-25 s 的绘制期间占用帧缓冲。没有这一步，手机
    // 可能 BEGIN_IMAGE 并把图片流进正被推往
    // 面板的那段字节里（而且 BEGIN_IMAGE 甚至不会回 ERR_BUSY）。
    if (!bleBeginHintPaint()) {
        BLE_LOG("[MAIN] hint not painted (%s): panel buffer busy\n", why);
        return false;
    }

    ensurePanelGpio();
    BLE_LOG("[MAIN] painting hint screen (%s)\n", why);
    ledSetMode(LedMode::SlowBlink);
    bleNotifyRendering();          // 解释快速 App 可能看到的 ERR_BUSY
    const bool ok = bleScreenShowHint(s_frame, s_deviceName);

    // 缓冲仍被占用时记录这次刷新（与图片路径顺序一致），
    // 然后释放它。其间到达的 BEGIN_IMAGE 绝不能
    // 依据过期的冷却状态被接受。
    refreshGuardNoteRefresh(opcode);
    bleRenderFinished();

    if (ok) {
        s_hintShown = true;        // 自动提示屏已经画过
        s_errorHold = false;
    } else {
        // 不要把提示屏标记为已显示：空闲定时器可以在
        // 冷却再次过去之后重试。
        s_errorHold = true;
        s_errorSince = millis();
        ledSetMode(LedMode::FastBlink);
    }

    bleNotifyState(ok ? BLE_STATE_HINT_SHOWN : BLE_STATE_ERROR,
                   ok ? BLE_OK : BLE_ERR_PANEL);
    return true;
}

// 丢弃一个待处理的提示屏请求及其记账信息。每条完成、
// 替换或放弃延迟提示屏的路径都需要清掉同样四个字段，
// 集中在一处正是防止它们彼此漂移的做法。
static void clearHintRequest() {
    s_hintWanted = false;
    s_hintWhy = nullptr;
    s_hintOpcode = 0;
    s_hintAuto = false;
}

// 请求提示屏，并遵守面板的冷却。
//
// 自动路径（空闲截止时间、BOOT 按键）没有 App 可问，所以来得太早的
// 提示屏就只是等待。App 请求的提示屏已经走过
// 确认握手，所以只有为 SHOW_HINT 签发的授权才能放行它
//（操作码绑定）。
//
// `automatic` 标记的是唯一一个前提会在等待期间过期的请求 -
// 空闲截止时间（无人连接）。BOOT 按键出于与空闲路径相同的原因
// 传 0 作为它的操作码（没有命令被拒绝，因此没有授权能适用于它），
// 但它是用户的有意动作，所以不算自动。
static void requestHint(const char *why, uint8_t opcode, bool automatic) {
    if (!s_frame) return;

    // 放行始终是守卫的职责：自动提示屏（操作码 0）必须
    // 等冷却过去，而 App 的提示屏只能使用为 SHOW_HINT
    // 签发的授权。绝不要在此检查之前调用 paintHint()。
    if (refreshGuardMayRefreshFor(opcode) && paintHint(why, opcode)) {
        clearHintRequest();
        return;
    }

    // 已经在等待的请求不会被后来的请求替换。空闲
    // 截止时间并没有重启，所以覆盖它的 why/opcode 会让一个
    // 新请求在冷却一结束就立刻触发 - 而这正是面板
    // 绝不能遇到的事。保留原条目也让日志保持诚实。
    if (s_hintWanted) {
        BLE_LOG("[MAIN] hint already deferred (%s), keeping it over (%s)\n",
                s_hintWhy ? s_hintWhy : "?", why);
        return;
    }

    s_hintWanted = true;
    s_hintWhy = why;
    s_hintOpcode = opcode;
    s_hintAuto = automatic;
    if (refreshGuardMayRefreshFor(opcode)) {
        BLE_LOG("[MAIN] hint deferred (%s): panel buffer busy\n", why);
    } else {
        BLE_LOG("[MAIN] hint deferred (%s): cool-down, %lu s left\n", why,
                (unsigned long)refreshGuardWaitSeconds());
    }
}

static void paintImage(uint8_t format, uint32_t bytes) {
    if (!s_frame) {
        // 防御性：bleTakeRenderRequest() 本应在没有缓冲区时
        // 拒绝占用它。
        bleRenderFinished();
        return;
    }

    // 图片永远优先于正在等冷却的提示屏：
    // 之后再画提示屏会擦掉用户刚发来的图片。
    clearHintRequest();

    // 缓冲里已经是面板原生布局的图片：ble_service
    // 在接收时已经把它从手机的竖屏画布旋转过来了。这里
    // 没有什么要转换，只有要推送的内容。
    BLE_LOG("[MAIN] painting image: format %u, %lu canvas bytes\n",
            (unsigned)format, (unsigned long)bytes);
    ensurePanelGpio();

    ledSetMode(LedMode::SlowBlink);
    bleNotifyRendering();

    epd398Display2bpp(s_frame);
    const bool ok = epd398LastOperationOk();

    // 启动冷却，并且如果 App 为这张图片走了警告对话框，
    // 就消耗掉确认授权。在告知 App DISPLAYED 之前完成，
    // 这样一个听到消息瞬间就发起刷新的 App，
    // 已经处在新的冷却期内。
    refreshGuardNoteRefresh(BLE_OP_BEGIN_IMAGE);

    // 在宣布结果之前把帧缓冲交回去，这样一个看到 DISPLAYED
    // 瞬间就开始下一张图的 App 不会无缘无故被回
    // ERR_BUSY。
    bleRenderFinished();

    // 只有真正上到屏的图片才抑制空闲提示屏
    // 并算作“图片已在屏上”。刷新失败时面板保持不变，
    // 所以提示屏仍然可用。
    if (ok) {
        s_imageShown = true;
        s_hintShown = true;
        s_errorHold = false;
    }

    bleNotifyDisplayed(ok ? BLE_OK : BLE_ERR_PANEL);
    if (ok) {
        BLE_LOG("[MAIN] picture displayed\n");
        ledFeedback("ack");
    } else {
        BLE_LOG("[MAIN] panel refresh failed (BUSY timeout)\n");
        s_errorHold = true;
        s_errorSince = millis();
        ledSetMode(LedMode::FastBlink);
    }
}

// GPIO9 短按 -> 重绘提示屏；长按 -> 重启。
static void checkButton() {
    const bool down = (digitalRead(PIN_CFG_BTN) == LOW);
    const unsigned long now = millis();

    if (down && !s_btnDown) {
        s_btnDown = true;
        s_btnDownAt = now;
        s_btnFired = false;
    } else if (down && s_btnDown && !s_btnFired) {
        if (now - s_btnDownAt >= CFG_BTN_REBOOT_MS) {
            s_btnFired = true;      // 只触发一次，无论按键被按多久
            BLE_LOG("[BTN] held %lums -> restart\n", now - s_btnDownAt);
            ledFeedback("off");
            delay(50);
            ESP.restart();
        }
    } else if (!down && s_btnDown) {
        const unsigned long held = now - s_btnDownAt;
        s_btnDown = false;
        if (!s_btnFired && held >= BTN_DEBOUNCE_MS) {
            BLE_LOG("[BTN] short press (%lums) -> hint screen\n", held);
            requestHint("button", 0, false);
        }
    }
}

static bool errorHoldActive() {
    if (!s_errorHold) return false;
    if ((unsigned long)(millis() - s_errorSince) < 10000UL) return true;
    s_errorHold = false;   // 抗回绕：用减法，而不是 millis() < until
    return false;
}

static void updateLed() {
    if (errorHoldActive()) return;   // 一段时间内由故障独占 LED
    if (bleIsTransferring()) {
        ledSetMode(LedMode::FastBlink);
    } else if (bleIsConnected()) {
        ledSetMode(LedMode::Solid);
    } else {
        ledSetMode(LedMode::SlowBlink);
    }
}

// ---------------------------------------------------------------- 初始化 / 主循环

void setup() {
    ledInit();

    Serial.begin(115200);
    delay(1500);                 // 让 USB CDC 控制台起来
    Serial.println("\n=== 8adge BLE (no WiFi) ===");
    Serial.printf("[MAIN] protocol v%u, firmware %u.%u.%u\n",
                  (unsigned)BLE_PROTOCOL_VERSION,
                  (unsigned)FW_VERSION_MAJOR, (unsigned)FW_VERSION_MINOR,
                  (unsigned)FW_VERSION_PATCH);

    pinMode(PIN_CFG_BTN, INPUT_PULLUP);

    // 为本次上电周期重置刷新冷却。在任何东西可能绘制之前调用，
    // 而正是它让开机后的第一次不安全刷新变得
    // 不可能：此时还没有时钟，守卫只能假设上一次刷新
    // 就发生在断电前的那一刻。
    refreshGuardInit();

    // 在 BLE 协议栈占走它那份堆之前分配。这是
    // 固件中最大的一次单笔分配，而且它必须是连续的。
    s_frame = epd398AllocBuffer();
    if (!s_frame) {
        Serial.println("[MAIN] FATAL: 105984-byte framebuffer allocation failed");
    }

    buildDeviceName();
    if (!bleInit(s_deviceName, s_frame)) {
        Serial.println("[MAIN] WARNING: no framebuffer, image transfers will be refused");
    }

    s_idleSince = millis();
    s_wasConnected = false;
    ledFeedback("waiting");

    Serial.printf("[MAIN] hint screen in %lu s if nothing connects\n",
                  (unsigned long)(BLE_HINT_DELAY_MS / 1000UL));
    Serial.println("[MAIN] panel left untouched on purpose: e-ink keeps the last picture");
}

void loop() {
    ledTick();
    blePollTransferTimeout();

    // 链路状态切换：断开连接会重启空闲倒计时，所以一台
    // 连接过又离开的手机，两分钟后仍会触发提示屏。
    const bool connected = bleIsConnected();
    if (s_wasConnected && !connected) {
        s_idleSince = millis();
        BLE_LOG("[MAIN] link idle, hint countdown restarted\n");
    }
    s_wasConnected = connected;

    uint8_t format = 0;
    uint32_t bytes = 0;
    // 一次绘制会让 loop 阻塞 15-25 s，所以其间到达的提示屏请求
    // 会等到下一轮迭代，而不是紧接着绘制 -
    // 连续两次整屏刷新会让设备有 50 s 不可用。
    if (bleTakeRenderRequest(&format, &bytes)) {
        paintImage(format, bytes);
    } else if (bleTakeHintRequest()) {
        requestHint("app request", BLE_OP_SHOW_HINT, false);
    }

    // 必须等冷却（或缓冲区）的提示屏，会在
    // 安全的那一刻发出。操作码被保留下来，所以为
    // SHOW_HINT 给出的 App 确认不会丢失，反之亦然。
    //
    // 自动（空闲截止时间）请求一旦前提消失就被丢弃：
    // 空闲截止时间之后有手机连上，意味着不再有人等着看
    // 说明，而且 App 可能已经选择关闭自动提示屏。
    // 那时再画它，只会在用户马上要做的上传之前烧掉冷却。
    // BOOT 按键和 App 请求是显式的
    //（s_hintAuto 为 false），所以它们继续等待。
    if (s_hintWanted && s_hintAuto &&
        (connected || (bleOptions() & BLE_OPT_DISABLE_HINT))) {
        BLE_LOG("[MAIN] deferred hint (%s) dropped: %s\n",
                s_hintWhy ? s_hintWhy : "?",
                connected ? "a phone connected" : "automatic hint disabled by the app");
        clearHintRequest();
    }

    if (s_hintWanted && s_frame && !bleIsTransferring() &&
        refreshGuardMayRefreshFor(s_hintOpcode)) {
        const char *why = s_hintWhy ? s_hintWhy : "deferred";
        const uint8_t op = s_hintOpcode;
        const bool automatic = s_hintAuto;
        clearHintRequest();
        if (paintHint(why, op)) {
            // 已绘制；不再有待处理项。
        } else {
            // 在争抢帧缓冲时输给了图片传输，所以该
            // 请求必须存活过它，来源信息也要保留。
            s_hintWanted = true;
            s_hintWhy = why;
            s_hintOpcode = op;
            s_hintAuto = automatic;
        }
    }

    checkButton();

    // 2 分钟规则。图片已在屏上时抑制（永不覆盖
    // 用户的图片），已有请求待处理时抑制（否则整个冷却期间
    // 每 5 ms 就会写一条日志），以及 App 通过 SET_OPTIONS
    // 选择关闭时抑制。
    if (!s_hintShown && !s_imageShown && !s_hintWanted && !connected &&
        !(bleOptions() & BLE_OPT_DISABLE_HINT) &&
        (millis() - s_idleSince) >= BLE_HINT_DELAY_MS) {
        requestHint("idle 2 min", 0, true);
    }

    updateLed();
    delay(5);
}
