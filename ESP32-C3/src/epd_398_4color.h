#ifndef EPD_398_4COLOR_H
#define EPD_398_4COLOR_H

#include <Arduino.h>

// ============================================================================
//  3.98 英寸 4 色墨水屏驱动 - A1 面板（黑 / 白 / 红 / 黄）
//
//  面板：      768 x 552 像素，每像素 2 bit（4 色）
//  控制器：    JD79668 系列，用 JD79665 寄存器组驱动
//  总线：      4 线软件模拟（bit-banged）SPI（MSB 在前，时钟空闲为低）
//  版本：      仅支持 A1 量产版本
//
//  单路径最终版。下面这些推理保留下来，是因为这里两处看起来很明显的
//  「改进」都会让画面更差，而且这两处都很容易从这个寄存器序列所源自的
//  开源驱动里被重新发明出来。
//
//  ---------------------------------------------------------------- 寻址
//
//  控制器的栅极地址空间是 600 线，但面板只有 552 个像素行，而且它的栅极线
//  按两组交错接线：
//
//      图像第   0..299 行  ->  栅极地址 0, 2, 4, ..., 598   （300 行）
//      图像第 300..551 行  ->  栅极地址 1, 3, 5, ..., 503   （252 行）
//
//  因此当 r <= 299 时图像第 r 行位于栅极 2*r，超过之后位于 2*r - 599，
//  并且必须把 0x61 的 VRES 设为 600，而不是 552。
//
//  当 VRES = 552 时，控制器只驱动栅极 0..551，因此图像第 276..299 行
//  （栅极地址 552..598）根本不会被驱动：画面从中间被切成两半，下半部分被
//  往下推，而那处空档一直显示上一帧。这正是开源驱动在这块面板上的故障，
//  也是它自己的 VRES = 552 不能照搬到这里的原因。
//
//  余下的 48 根栅极（奇数 505..599）没有对应的像素行。控制器成对地驱动
//  这两组交错的栅极，所以 504..598 这些栅极各自与一个悬空的伙伴配对，
//  扫描的最后 96 根栅极只被加载了一半。这就是过去让图像第 252..299 行
//  （屏幕自上而下 45.7%-54.3% 处）明显比其余部分更暗的原因 - 在单一颜色的
//  大片平坦区域上最明显，那里整条带会偏到更深的色调。
//
//  ---------------------------------------------------------------- 寄存器序列
//
//  寄存器序列是开源华为 A1 适配版
//  （github.com/krstc/InkSight_adapt_HUAWEI_eink, MIT）里的 JD79665 那一套。
//  正是它消除了那条暗带：厂商固件自己的序列把 0x00 设为 <- 0x0B，
//  「LUT from OTP」，这会让面板停留在一种会对只加载了一半的栅极对起反应的
//  内部波形上。
//
//  寄存器组和寻址方式是耦合的，所以两者都不能单独替换：厂商寄存器配的是
//  交错寻址，而开源寄存器是为线性寻址写的。在本面板上实测，除本文件之外的
//  每一种组合都是错的：
//
//      厂商寄存器       + VRES 600 + 552 个单行窗口
//          -> 画面正确，但 45.7%-54.3% 处的暗带存在
//      开源寄存器 + VRES 600 + 552 个单行窗口
//          -> 画面在垂直方向被拉伸约 2 倍，下半部分跑出面板之外
//      开源寄存器 + VRES 552 + 线性 burst
//          -> 50.0%-54.3%（图像第 276..299 行）处出现 24 行死带
//      开源寄存器 + VRES 600 + 单次 burst           <- 本文件
//          -> 画面正确，没有暗带
//
//  一次贯穿全窗口的 0x10 burst 依然会按交错顺序走过栅极空间，因此帧缓冲的
//  顺序（图像第 0..551 行）会落到正确的栅极上，驱动不必再去发 552 个单行的
//  0x83 窗口。
//
//  ---------------------------------------------------------------- 刷屏
//
//  BUSY 可能在波形结束之前就变为高电平，所以少于 EPD398_BUSY_EARLY_MS 的
//  释放会被当成「尚未结束」，并在断电前补上一段固定的 EPD398_REFRESH_FALLBACK_MS
//  等待。
//
//  这段兜底等待必须超过最长的真实刷屏时间（15-25 s）。更短的固定等待会在
//  最后几行还在写入时就给面板断电，那些行会因此过驱。开源驱动这里用的是
//  14000 ms，落在该区间内，因此不安全；这里改用 25000 ms。0x12 取值为 0x00，
//  与开源驱动一致。
//
//  ---------------------------------------------------------------- 命令
//
//      0x00 面板设置        0x04 电源开          0x12 显示刷屏
//      0x02 电源关          0x07 深度睡眠        0x61 分辨率
//      0x10 开始传输数据      0x83 局部窗口
//
//  本文件是自包含的：如果宿主工程已经定义了 PIN_EPD_* 宏（例如 8adge 的
//  config.h），就用那些定义，否则套用下面的默认值（厂商接线）。
// ============================================================================

// ---------------------------------------------------------------- 几何
#define EPD398_WIDTH        768
#define EPD398_HEIGHT       552
// 每像素 2 bit，面板的原生打包方式：每字节 4 个像素，最高位那对在最左边。
// 在 GET_INFO 里上报给 App。
#define EPD398_BPP          2
#define EPD398_ROW_BYTES    (EPD398_WIDTH / 4)                  // 192 字节/行 @ 2bpp
#define EPD398_COLOR_BYTES  (EPD398_WIDTH * EPD398_HEIGHT / 4)  // 105984 字节

// 控制器栅极地址空间。是 600，而不是 EPD398_HEIGHT - 见上面的寻址说明。
// 552 线的空间无法到达图像第 276..299 行的栅极地址，那是可见的缺陷，而不是
// 舍入误差。
#define EPD398_VRES         600

// 两个边界都是编译期的，因为它们防范的故障是静默的：当 VRES = 552 时，
// 控制器根本不驱动承载图像第 276..299 行的栅极（一半画面被下移，并且留下
// 24 行显示上一帧），而比控制器自身的 600 线空间更大的 VRES 只会把扫描时间
// 浪费在寻址根本不存在的栅极上。
//      图像第   0..299 行 -> 栅极 0, 2, 4, ..., 598   （最高：598）
//      图像第 300..551 行 -> 栅极 1, 3, 5, ..., 503
static_assert(EPD398_VRES >= 599,
              "EPD398_VRES must reach gate 598 (image row 299); 552 leaves "
              "image rows 276..299 undriven");
static_assert(EPD398_VRES <= 600,
              "EPD398_VRES exceeds the controller's 600-line gate space");

// ---------------------------------------------------------------- 引脚
// 厂商接线（屏侧 -> ESP32-C3 侧）：
//   DIN -> 6   SCK -> 4   DC -> 1   CS -> 7   RST -> 2   BUSY -> 10
#ifndef PIN_EPD_MOSI
#define PIN_EPD_MOSI 6
#endif
#ifndef PIN_EPD_SCK
#define PIN_EPD_SCK  4
#endif
#ifndef PIN_EPD_DC
#define PIN_EPD_DC   1
#endif
#ifndef PIN_EPD_CS
#define PIN_EPD_CS   7
#endif
#ifndef PIN_EPD_RST
#define PIN_EPD_RST  2
#endif
#ifndef PIN_EPD_BUSY
#define PIN_EPD_BUSY 10
#endif

// ---------------------------------------------------------------- 时序
// 电源开 / 电源关握手的 BUSY 轮询超时（厂商固件用的是 45000 ms）。
#ifndef EPD398_BUSY_TIMEOUT_MS
#define EPD398_BUSY_TIMEOUT_MS 45000UL
#endif

// 显示刷屏本身的 BUSY 超时。这块面板上一次 4 色整屏刷新通常要 15-25 s。
// 开源驱动允许 180 s，但那是面板一直不回应时主循环被阻塞三分钟，所以这里
// 用 60 s（仍然约为预期刷屏时间的 2.5 倍）。
#ifndef EPD398_REFRESH_TIMEOUT_MS
#define EPD398_REFRESH_TIMEOUT_MS 60000UL
#endif

// 硬件复位后等待 BUSY 释放的时间。面板通常几百毫秒内就会应答，所以 10 s
// 很充裕，而且不会让一块坏掉的面板拖延启动。
#ifndef EPD398_RESET_WAIT_MS
#define EPD398_RESET_WAIT_MS 10000UL
#endif

// BUSY 释放得比这个还快，就意味着「波形尚未结束」，于是驱动改为等待
// EPD398_REFRESH_FALLBACK_MS，而不是断电。设为 0 则关闭该检查。
#ifndef EPD398_BUSY_EARLY_MS
#define EPD398_BUSY_EARLY_MS 250UL
#endif

// BUSY 提前释放时用来替代的等待时间。必须超过最长的真实刷屏时间（15-25 s），
// 否则它会在波形中途给面板断电，最后几行会过驱。开源驱动的 14000 ms 太短。
#ifndef EPD398_REFRESH_FALLBACK_MS
#define EPD398_REFRESH_FALLBACK_MS 25000UL
#endif

// 一次看起来正常的刷屏之后、断电之前额外加的稳定时间。
#ifndef EPD398_REFRESH_SETTLE_MS
#define EPD398_REFRESH_SETTLE_MS 500UL
#endif

// 0x12 显示刷屏命令的数据字节。0x00 属于开源寄存器序列；厂商序列用的是
// 0x01。实测：这个字节不影响那条暗带，但它确实属于该序列。
#ifndef EPD398_REFRESH_MODE
#define EPD398_REFRESH_MODE 0x00
#endif

// 设为 0 可关闭驱动的串口日志。
#ifndef EPD398_DEBUG
#define EPD398_DEBUG 1
#endif

// ---------------------------------------------------------------- 颜色
// 2bpp 颜色码。0x00/0x01 遵循开源版
// packMonoPixelGroup() 的约定；0x02 在 epd_driver.cpp 里由注释
// `if (color == 0x02) color = 0x01; // 把黄色映射为红色` 记录为黄色。
// 请用 epd398TestPattern() 在自己的面板上核对。
#define EPD398_BLACK  0x00
#define EPD398_WHITE  0x01
#define EPD398_YELLOW 0x02
#define EPD398_RED    0x03

// ---------------------------------------------------------------- API
// 低层
void epd398GpioInit();
void epd398WaitBusy(unsigned long timeoutMs = EPD398_BUSY_TIMEOUT_MS);

// 与 epd398WaitBusy() 相同，但会报告面板是否真的释放了 BUSY。当调用方需要
// 区分真实刷屏与超时时用它（epd398Display2bpp() 就是这么做的）。
bool epd398WaitBusyTimed(unsigned long timeoutMs);

void epd398Init();
void epd398PowerOff();
void epd398Sleep();

// 绘制一个 2bpp 帧缓冲（EPD398_COLOR_BYTES 字节）并刷新面板。
// 完整流程：init -> 上传 -> 0x12 刷屏 -> 断电。
void epd398Display2bpp(const uint8_t *colorBuf);

// 相同，但省去 init/断电步骤，供自行管理这些步骤时使用。
void epd398Upload2bpp(const uint8_t *colorBuf);

// 最近一次 epd398Display2bpp() 完成了刷屏（BUSY 释放，或执行了兜底等待）时
// 为 true。BUSY 超时之后为 false。
bool epd398LastOperationOk();

// ---------------------------------------------------------------- 辅助函数
// 在堆上分配帧缓冲（105984 字节）。失败时返回 nullptr。用
// epd398FreeBuffer() 释放。
uint8_t *epd398AllocBuffer();

// 释放由 epd398AllocBuffer() 取得的缓冲区。
void epd398FreeBuffer(uint8_t *buf);

// 用同一种颜色码填充整个帧缓冲。
void epd398Fill(uint8_t *buf, uint8_t color);

// 设置一个像素。面板之外的坐标会被忽略。
void epd398SetPixel(uint8_t *buf, int x, int y, uint8_t color);

// 填充一个矩形。
void epd398FillRect(uint8_t *buf, int x, int y, int w, int h, uint8_t color);

// 用垂直色条（每种颜色一条）加四角标记填充帧缓冲，
// 用来核对自己面板上的颜色映射。
void epd398TestPattern(uint8_t *buf, const uint8_t *colors, int count);

#endif // EPD_398_4COLOR_H
