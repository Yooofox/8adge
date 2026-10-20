// 板载状态 LED：一个小巧的、非阻塞的状态机。
//
// 本设备是“无头”小玩意，唯一的反馈通道就是一颗蓝色 LED
//（C3-SuperMini 上的 GPIO8，低电平有效 - 见 config.h 里的 PIN_LED），
// 所以这颗 LED 必须同时承载四种含义：
//
//   Solid       “一切正常”     - 手机已连接，或图片已在屏上
//   SlowBlink   “正在处理”     - 正在广播，或正在刷屏
//   FastBlink   “需要关注”     - 传输进行中，或出错
//   Off         “休眠”
//
// 两个入口点让调用点保持简单：
//
//   ledSetMode(mode)     设置稳态
//   ledFeedback(pattern) 一次性确认；“waiting”和“off”也会改变
//                        稳态，所以调用点不必记住
//                        要同时做这两件事。可用模式见下面的声明。
//
// 这里没有阻塞操作：ledTick() 基于 millis() 推进闪烁，ledDelay()
// 是能持续跳动的 delay() 替代品。正是 ledDelay() 让 LED 在面板
// 于一次刷新中忙上 25 s 时仍能继续闪烁。
#pragma once

#include <stdint.h>

enum class LedMode : uint8_t {
    Off,        // 稳定熄灭 - 休眠
    Solid,      // 稳定点亮 - 就绪
    FastBlink,  // ~5 Hz      - 故障 / 等待输入
    SlowBlink,  // ~0.8 Hz    - 忙
};

/** 配置引脚并让 LED 停在熄灭状态。可在其他任何事之前安全调用。 */
void ledInit();

/** 设置稳态。模式已生效时为空操作。 */
void ledSetMode(LedMode mode);

/** 当前稳态。 */
LedMode ledMode();

/** 推进一次闪烁。开销很小；请在每次 loop 迭代中调用它。 */
void ledTick();

/** 会持续 ledTick() 的 delay()，让闪烁能熬过长时间阻塞等待。 */
void ledDelay(unsigned long ms);

/**
 * 一次性模式："ack"（两次短闪，不动稳态）、
 * "waiting"（开始慢闪）或 "off"。未知模式会被忽略并
 * 保持 LED 不变，这样一个过期的调用点无法把它停在
 * 一个再也不会被改变的状态里。
 */
void ledFeedback(const char *pattern);
