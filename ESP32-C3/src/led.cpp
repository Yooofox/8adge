#include "led.h"

#include <Arduino.h>
#include <string.h>

#include "config.h"

// 板载蓝色 LED，低电平有效（见 config.h）。每次写引脚都走这两个
// 宏，这样极性只存在于一处；而且当 PIN_LED < 0 时两者都编译成
// 空操作 - ledFeedback() 会直接写引脚，所以只给
// ledApplyPin() 加门控会留下一个行为与其注释不符的
// 配置开关。
#if PIN_LED >= 0
#define LED_ON()   digitalWrite(PIN_LED, PIN_LED_ACTIVE_LOW ? LOW : HIGH)
#define LED_OFF()  digitalWrite(PIN_LED, PIN_LED_ACTIVE_LOW ? HIGH : LOW)
#else
#define LED_ON()   do {} while (0)
#define LED_OFF()  do {} while (0)
#endif

namespace {

// 半周期。快闪是“出问题了”的急促抖动，慢闪是沉稳的
// “正在工作”脉动，在面板刷新 25 s 期间也依然清晰可辨。
constexpr unsigned long FAST_HALF_MS = 100;   // ~5 Hz
constexpr unsigned long SLOW_HALF_MS = 600;   // ~0.8 Hz

LedMode g_mode = LedMode::Off;
bool g_lit = false;
unsigned long g_lastToggleAt = 0;

void ledApplyPin() {
    if (g_lit) {
        LED_ON();
    } else {
        LED_OFF();
    }
}

unsigned long ledHalfPeriod(LedMode mode) {
    switch (mode) {
        case LedMode::FastBlink: return FAST_HALF_MS;
        case LedMode::SlowBlink: return SLOW_HALF_MS;
        default:                 return 0;
    }
}

// 改变稳态，并让引脚处于确定的相位：Solid/Off
// 立即稳定，闪烁则从点亮开始，这样状态切换立刻可见。
void ledApplyMode(LedMode mode, bool restartPhase) {
    g_mode = mode;
    g_lit = (mode != LedMode::Off);
    if (restartPhase) {
        g_lastToggleAt = millis();
    }
    ledApplyPin();
}

}  // namespace

void ledInit() {
#if PIN_LED >= 0
    pinMode(PIN_LED, OUTPUT);
#endif
    g_mode = LedMode::Off;
    g_lit = false;
    LED_OFF();   // GPIO8 是 strapping 引脚：让它停在 OFF，绝不 ON
}

void ledSetMode(LedMode mode) {
    if (mode == g_mode) return;
    ledApplyMode(mode, true);
}

LedMode ledMode() {
    return g_mode;
}

void ledTick() {
    unsigned long half = ledHalfPeriod(g_mode);
    if (half == 0) return;   // Off / Solid 不需要做任何事
    unsigned long now = millis();
    if (now - g_lastToggleAt < half) return;
    g_lastToggleAt = now;
    g_lit = !g_lit;
    ledApplyPin();
}

void ledDelay(unsigned long ms) {
    unsigned long startedAt = millis();
    while (millis() - startedAt < ms) {
        ledTick();
        // 面板刷新会通过这个助手阻塞 loop 15-25 s。
                // delay() 会让出 FreeRTOS，而正是这一点让 BLE host 任务及其
                // 连接在刷屏期间保持存活。
        delay(5);
    }
    ledTick();
}

void ledFeedback(const char *pattern) {
    if (!pattern) return;

    LedMode next = g_mode;

    if (strcmp(pattern, "ack") == 0) {
        for (int i = 0; i < 2; i++) {
            LED_ON();  delay(80);
            LED_OFF(); delay(80);
        }
    } else if (strcmp(pattern, "waiting") == 0) {
        // 正在广播且还没有手机连接：一段沉稳、不易忽略的脉动。
        next = LedMode::SlowBlink;
    } else if (strcmp(pattern, "off") == 0) {
        next = LedMode::Off;
    } else {
        // 未知模式：保持 LED 不变。上面三种是本固件
                // 唯一使用的；头文件过去列出的 "connecting"/"success"/"fail"
                // 系列来自 WiFi 构建，已经没有调用点，
                // 所以直接删除，而不是留作描述不了任何东西的
                // 文档。
        return;
    }

    // 重新施加稳态，并重启闪烁相位，这样引脚绝不会
        // 停留在上一次闪烁留下的电平上。
    ledApplyMode(next, true);
}
