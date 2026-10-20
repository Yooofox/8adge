#ifndef BADGE8_BLE_SERVICE_H
#define BADGE8_BLE_SERVICE_H

// ============================================================================
//  固件的 BLE 一侧：NimBLE GATT 服务器 + 协议引擎。
//
//  手机能对这台设备做的一切都在这里实现；main.cpp 只负责那些绝不能放在
//  BLE 回调里跑的事情（绘制面板、驱动 LED、2 分钟提示屏定时器）。
//
//  调用流程：
//      bleInit(name, frame)        一次，来自 setup()
//      ... App 传输期间，BLE 回调在 NimBLE 任务上触发 ...
//      bleTakeRenderRequest(...)   从 loop() 轮询；每次传输返回一次 true
//      bleNotifyRendering()/bleNotifyDisplayed()   来自 loop()，围绕绘制
// ============================================================================

#include <Arduino.h>
#include <stdint.h>

// 启动协议栈，注册 GATT 服务并开始广播。
//
// `deviceName` 原样广播，并在 GET_INFO 的回复中回显。
// `frameBuffer` 必须指向 EPD398_COLOR_BYTES（105984）字节的内存，并在固件的
// 整个生命周期内存活：DATA 写入直接落在其中，这正是让传输不必再有一份
// 106 KB 副本的原因。
//
// 缓冲区为空时返回 false（协议栈仍会启动，但每个 BEGIN_IMAGE 都会以 ERR_MEM
// 作答）。
bool bleInit(const char *deviceName, uint8_t *frameBuffer);

// 传给 bleInit() 的名字。
const char *bleDeviceName();

// ---------------------------------------------------------------- 状态
bool bleIsConnected();
// 自开机以来有任何中心设备连接过即为 true。仅供参考：空闲提示屏定时器用的是
// bleIsConnected()，而不是这个。
bool bleHasEverConnected();
uint32_t bleConnectionCount();

// ---------------------------------------------------------------- 传输
// 一次 BEGIN_IMAGE 传输进行中（帧缓冲正在被使用）期间为 true。
bool bleIsTransferring();

// App 正在重传单个块（RETRY_BLOCK 已武装）期间为 true。
bool bleIsFixingBlock();

// 一次通过校验的传输完成后为 true，每幅图像恰好一次。
// 它会消费该标志并把帧缓冲标记为正在使用，因此调用方在面板绘制完成后
// 必须配对调用 bleRenderFinished()。
bool bleTakeRenderRequest(uint8_t *outFormat, uint32_t *outBytes);

// 绘制完成后释放帧缓冲。在它被调用之前，BEGIN_IMAGE 会以 ERR_BUSY 作答 -
// 整屏刷新需要 15-25 s，在此期间开始的第二次传输会覆写正在送往面板的字节。
void bleRenderFinished();

// 为一次设备自绘的屏幕（提示屏）占用帧缓冲。当一次传输正在接收、一幅已通过
// 校验的图像仍在等待绘制、或另一次绘制已经持有该缓冲区时，返回 false 且不做
// 任何改动；此时调用方绝不能触碰帧缓冲。成功时调用方必须配对调用
// bleRenderFinished()。
//
// 没有这个机制，提示屏路径就可能在手机正向同一个 105984 字节缓冲区流入
// 图像时绘制（而且 BEGIN_IMAGE 甚至不会看到 ERR_BUSY）。
bool bleBeginHintPaint();

// 在链路仍然保持的情况下，放弃一次 App 在第一个 DATA 字节之后静默
// BLE_RX_TIMEOUT_MS 的传输（或者 App 仍在准备载荷时，BEGIN_IMAGE 之后静默
// BLE_BEGIN_TIMEOUT_MS 的传输）。从 loop() 轮询它；它把 RECEIVING/FIXING
// 重置回 CONNECTED 并发出 EVT_STATE(CONNECTED)，这样下一次 BEGIN_IMAGE 就不会
// 永远得到 ERR_STATE 的答复。
void blePollTransferTimeout();

// App 请求提示屏（SHOW_HINT）之后恰好一次为 true。
bool bleTakeHintRequest();

// 最后一次 SET_OPTIONS 设置的选项位（新连接之后为 0）。
uint8_t bleOptions();

// ---------------------------------------------------------------- 事件
// 向已连接的 App 推送一个 EVT_STATE 帧。从主循环调用是安全的。
void bleNotifyState(uint8_t state, uint8_t error);
void bleNotifyRendering();
void bleNotifyDisplayed(uint8_t error);
// 推送一个 EVT_LOG 帧（UTF-8，无 NUL）。为调试 App 提供的便利。
void bleNotifyLog(const char *text);

#endif // BADGE8_BLE_SERVICE_H
