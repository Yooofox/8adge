// ============================================================================
//  3.98 英寸 4 色墨水屏驱动 - A1 面板（768x552，黑/白/红/黄）。
//
//  面板的栅极线按两组交错接线，因此图像第 r 行位于控制器 600 线地址空间内的
//  栅极地址 2*r（r <= 299）或 2*r - 599（r >= 300）。所以 0x61 必须上报
//  VRES = 600。若为 552，控制器就无法到达承载图像第 276..299 行的栅极，
//  画面会从中间被切成两半。完整推理见头文件。
//
//  寄存器序列是来自
//  github.com/krstc/InkSight_adapt_HUAWEI_eink（MIT）的开源 JD79665 那一套，
//  正是它消除了厂商固件那套「LUT from OTP」序列在图像第 252..299 行上造成的
//  暗带。它的复位和它的 0x12 <- 0x00 被保留；它的 VRES = 552 没有保留
//  （正是那个值让 24 行得不到驱动）。
//
//  本文件是单路径最终版：没有编译期开关，因为每种替代方案都实测过，而且都
//  更差（清单见头文件）。
// ============================================================================

// 在 8adge 工程内构建时，从 config.h 取走板级引脚定义，
// 让驱动遵循工程的接线宏，而不是内建默认值。
#if defined(__has_include)
#  if __has_include("config.h")
#    include "config.h"
#  endif
// 在 8adge 工程里状态 LED 位于 led.h。这里和 config.h 一样做了保护，
// 这样本驱动仍可直接放进一个独立 sketch，那里没有 LED 模块，等待就退回
// 普通的 delay()。
#  if __has_include("led.h")
#    include "led.h"
#    define EPD398_HAS_LED 1
#  endif
#endif

#ifndef EPD398_HAS_LED
#  define EPD398_HAS_LED 0
#endif

#include "epd_398_4color.h"

// 长时间面板等待用的等待辅助函数。一次刷屏会把调用方占住几十秒；存在 LED
// 模块时，它能让状态 LED 在这期间持续闪烁，而不是冻结在之前留下的状态上。
static void epd398WaitTick(unsigned long ms) {
#if EPD398_HAS_LED
    ledDelay(ms);
#else
    delay(ms);
#endif
}

#if EPD398_DEBUG
#define EPD398_LOG(...) Serial.printf(__VA_ARGS__)
#else
#define EPD398_LOG(...) do {} while (0)
#endif

// 最近一次完整显示操作的结果，让调用方可以区分真实刷屏和一直不应答的面板。
// 见 epd398LastOperationOk()。
static bool s_lastOperationOk = false;

// 引脚在首次使用时才配置，所以即使宿主没有先调用 epd398GpioInit()，本驱动
// 也可以安全调用（8adge 工程有意把它推迟到第一次绘制时）。每个公开入口
// 都会调用 ensureGpio()；它只是配置引脚，因此绝不会打扰面板。
static bool s_gpioReady = false;
static void ensureGpio();

// ---------------------------------------------------------------- SPI 层
// 软件模拟的 4 线 SPI。MSB 在前，MOSI 在 SCK 上升前就绪，SCK 空闲为低。
// 已对照厂商在 0x42000FF4 处的 spiWriteByte 验证。
static void epd398SpiWriteByte(uint8_t data) {
    for (int i = 0; i < 8; i++) {
        digitalWrite(PIN_EPD_MOSI, (data >> 7) & 0x01);
        digitalWrite(PIN_EPD_SCK, HIGH);
        data = (uint8_t)(data << 1);
        digitalWrite(PIN_EPD_SCK, LOW);
    }
}

static void epd398SendCommand(uint8_t cmd) {
    digitalWrite(PIN_EPD_DC, LOW);      // DC 低 = 命令
    digitalWrite(PIN_EPD_CS, LOW);
    epd398SpiWriteByte(cmd);
    digitalWrite(PIN_EPD_CS, HIGH);
}

static void epd398SendData(uint8_t dat) {
    digitalWrite(PIN_EPD_DC, HIGH);     // DC 高 = 数据
    digitalWrite(PIN_EPD_CS, LOW);
    epd398SpiWriteByte(dat);
    digitalWrite(PIN_EPD_CS, HIGH);
}

// ---------------------------------------------------------------- BUSY 等待
// BUSY 低电平有效：等到它变为高电平。面板释放 BUSY 时返回 true，
// 超时返回 false。
//
// 使用 epd398WaitTick() 而不是 delay()，这样面板忙时状态 LED 会继续闪烁 -
// 一次刷屏会让这个循环持续几十秒。
bool epd398WaitBusyTimed(unsigned long timeoutMs) {
    ensureGpio();
    unsigned long t0 = millis();
    while (digitalRead(PIN_EPD_BUSY) == LOW) {
        epd398WaitTick(10);
        if (millis() - t0 > timeoutMs) {
            EPD398_LOG("[EPD398] BUSY TIMEOUT after %lums\n", timeoutMs);
            return false;
        }
    }
    return true;
}

void epd398WaitBusy(unsigned long timeoutMs) {
    (void)epd398WaitBusyTimed(timeoutMs);
}

// ---------------------------------------------------------------- GPIO 初始化
void epd398GpioInit() {
    pinMode(PIN_EPD_BUSY, INPUT);
    pinMode(PIN_EPD_RST,  OUTPUT);
    pinMode(PIN_EPD_DC,   OUTPUT);
    pinMode(PIN_EPD_CS,   OUTPUT);
    pinMode(PIN_EPD_SCK,  OUTPUT);
    pinMode(PIN_EPD_MOSI, OUTPUT);
    digitalWrite(PIN_EPD_RST, HIGH);
    digitalWrite(PIN_EPD_CS,  HIGH);
    digitalWrite(PIN_EPD_SCK, LOW);
    s_gpioReady = true;
}

// 如果宿主没有（或还没有）配置引脚，就配置它们。每一个公开入口点都会调用
// 它：头文件承诺这是个可以独立使用的驱动，而 epd398Init()/epd398Upload2bpp()
// 会立刻写 RST/DC/CS。它只设置引脚方向（RST 保持高电平），所以不会打扰面板 -
// 这正是宿主可以把它推迟到第一次绘制的原因。
static void ensureGpio() {
    if (!s_gpioReady) epd398GpioInit();
}

// ---------------------------------------------------------------- 窗口
// 命令 0x83 接收 9 个数据字节：
//   x_start(2) x_end(2) y_start(2) y_end(2) flag
// 只使用全屏窗口：一次 0x10 burst 写入整帧，控制器会自行按交错顺序走过它的
// 600 线栅极空间，因此不需要逐行窗口。
//
// 请仔细读上面这两句话，因为它们看起来矛盾：头文件里的「2*r / 2*r-599」表
// 说的是*面板*某行的栅极位于控制器地址空间的哪里，而 burst 是通过这个窗口
// 一个接一个地写入*图像*行。二者只有在下述条件下才一致：控制器自己为 0x10
// burst 服务的行计数器本来就是按帧缓冲存放的顺序走过这两组交错的栅极 - 这
// 正是这里 VRES 必须为 600 的原因（552 线的空间会停止驱动图像第 276..299 行
// 的栅极），也是把它换成 552 个单行 0x83 窗口或 VRES = 552 就会重现头文件里
// 列出的半幅/拉伸故障模式的原因。该映射是在 A1 面板上实测的；如果以后改动
// 寄存器序列，请先重新实测再信任它。
static void epd398SetWindow(uint16_t y0, uint16_t y1) {
    epd398SendCommand(0x83);
    epd398SendData(0x00); epd398SendData(0x00);          // x 起点 = 0
    epd398SendData(0x02); epd398SendData(0xFF);          // x 终点 = 767
    epd398SendData((uint8_t)(y0 >> 8)); epd398SendData((uint8_t)y0);
    epd398SendData((uint8_t)(y1 >> 8)); epd398SendData((uint8_t)y1);
    epd398SendData(0x01);
}

// 恢复控制器的全窗口（0..767 x 0..VRES-1）。0x10 burst 就是通过这个窗口
// 写入的。
static void epd398SetFullWindow() {
    epd398SetWindow(0, EPD398_VRES - 1);
}

// ---------------------------------------------------------------- 初始化
// 0x61 分辨率设置。VRES 是控制器的栅极地址空间，即 600 - 不是面板高度
// （552）。见头文件里的寻址说明：552 线的空间无法到达承载图像第 276..299 行
// 的栅极。
static void epd398SendResolution() {
    epd398SendCommand(0x61);                 // 分辨率设置
    epd398SendData((uint8_t)(EPD398_WIDTH >> 8));
    epd398SendData((uint8_t)(EPD398_WIDTH & 0xFF));    // HRES = 0x0300 = 768
    epd398SendData((uint8_t)(EPD398_VRES >> 8));
    epd398SendData((uint8_t)(EPD398_VRES & 0xFF));     // VRES = 0x0258 = 600
}

void epd398Init() {
    ensureGpio();
    // ---- JD79665 寄存器序列（krstc/InkSight_adapt_HUAWEI_eink, MIT）----
        // 取自参考驱动的 firmware/src/epd_driver.cpp，只改了一处：
    // 0x61 的 VRES 设为 600 而不是参考版的 552，因为参考版自己的取值会让图像
    // 第 276..299 行在这块面板上得不到驱动。其余全部按参考版原样复现 -
    // 包括复位（线拉高，然后 2 ms 低电平脉冲），它与厂商固件相反的顺序和长得
    // 多的延时不同。
    digitalWrite(PIN_EPD_RST, HIGH); delay(20);
    digitalWrite(PIN_EPD_RST, LOW);  delay(2);
    digitalWrite(PIN_EPD_RST, HIGH); delay(20);

    epd398WaitBusy(EPD398_RESET_WAIT_MS);
    delay(30);

    epd398SendCommand(0xAA);                 // CMDH / 面板专用设置
    epd398SendData(0x49);
    epd398SendData(0x55);
    epd398SendData(0x20);
    epd398SendData(0x08);
    epd398SendData(0x09);
    epd398SendData(0x18);

    epd398SendCommand(0x01);                 // 电源设置
    epd398SendData(0x3F);

    epd398SendCommand(0x00);                 // 面板设置（PSR）
    epd398SendData(0x4B);
    epd398SendData(0x69);

    epd398SendCommand(0x05);                 // 升压软启动
    epd398SendData(0x40);
    epd398SendData(0x1F);
    epd398SendData(0x1F);
    epd398SendData(0x2C);

    epd398SendCommand(0x08);                 // VCOM 直流设置
    epd398SendData(0x6F);
    epd398SendData(0x1F);
    epd398SendData(0x1F);
    epd398SendData(0x22);

    epd398SendCommand(0x06);                 // TSSET
    epd398SendData(0x6F);
    epd398SendData(0x1F);
    epd398SendData(0x1F);
    epd398SendData(0x14);
    epd398SendData(0x14);

    epd398SendCommand(0x03);                 // VCOM / 数据间隔（VDCS）
    epd398SendData(0x00);
    epd398SendData(0x54);
    epd398SendData(0x00);
    epd398SendData(0x44);

    epd398SendCommand(0x60);                 // TCON 设置
    epd398SendData(0x02);
    epd398SendData(0x00);

    epd398SendCommand(0x30);                 // PLL 控制
    epd398SendData(0x08);

    epd398SendCommand(0x50);                 // VCOM 与数据间隔设置
    epd398SendData(0x3F);

    epd398SendResolution();                  // 768 x 600（参考版：768 x 552）

    epd398SendCommand(0x65);                 // GSST
    epd398SendData(0x10);
    epd398SendData(0x00);
    epd398SendData(0x20);
    epd398SendData(0x00);

    epd398SendCommand(0xE3);                 // 省电
    epd398SendData(0x2F);

    epd398SendCommand(0x84);                 // VCOM 与数据间隔（第 2 处）
    epd398SendData(0x01);

    epd398SetFullWindow();                   // 0x83，0..767 x 0..599
    epd398SendCommand(0x04);                 // 电源开
    epd398WaitBusy(EPD398_BUSY_TIMEOUT_MS);

    EPD398_LOG("[EPD398] init done (VRES=%d, BUSY=%d)\n",
               (int)EPD398_VRES, digitalRead(PIN_EPD_BUSY));
}

    // ---------------------------------------------------------------- 上传
    // 2bpp 帧缓冲 -> 控制器 RAM，以一次贯穿全屏窗口的 0x10 burst 完成。控制器会按
    // 交错顺序走过它的 600 线栅极空间，因此帧缓冲的顺序（图像第 0..551 行）会落到
    // 正确的栅极上，驱动不必再去发 552 个单行的 0x83 窗口。
void epd398Upload2bpp(const uint8_t *colorBuf) {
    if (!colorBuf) {
        EPD398_LOG("[EPD398] upload skipped: null framebuffer\n");
        return;
    }
    ensureGpio();

    epd398SendCommand(0x04);      // 电源开
    epd398WaitBusy(EPD398_BUSY_TIMEOUT_MS);

    epd398SetFullWindow();
    epd398SendCommand(0x10);              // 开始传输数据
    for (int i = 0; i < EPD398_COLOR_BYTES; i++) {
        epd398SendData(colorBuf[i]);
    }
}

    // ---------------------------------------------------------------- 电源
void epd398PowerOff() {
    ensureGpio();
    epd398SendCommand(0x02);
    epd398SendData(0x00);
    epd398WaitBusy(EPD398_BUSY_TIMEOUT_MS);
}

void epd398Sleep() {
    ensureGpio();
    epd398SendCommand(0x07);
    epd398SendData(0xA5);
    delay(200);
}

// ---------------------------------------------------------------- 刷屏
// 启动显示更新并等待它真正结束。
//
// 关键细节：这块面板的 BUSY 可能在波形完成之前很早就回到高电平。只信它就意味着
// 在刷屏中途给面板断电，屏幕会一片空白。因此少于 EPD398_BUSY_EARLY_MS 的释放
// 会被当成「尚未结束」，并替换为一段固定的 EPD398_REFRESH_FALLBACK_MS 等待。
static bool epd398RefreshPanel() {
    // 按参考驱动在刷屏前的做法，重新装载全窗口。epd398Upload2bpp() 已经把它
    // 留在 0..599，所以实际上这是个空操作 - 保留它是因为序列其余部分都按
    // 参考版逐字复现。
    epd398SetFullWindow();

    epd398SendCommand(0x12);                 // 显示刷屏
    epd398SendData(EPD398_REFRESH_MODE);

    EPD398_LOG("[EPD398] refresh 0x12 <- 0x%02X\n", (unsigned)EPD398_REFRESH_MODE);

    unsigned long t0 = millis();
    bool busyOk = epd398WaitBusyTimed(EPD398_REFRESH_TIMEOUT_MS);
    unsigned long elapsed = millis() - t0;

    if (!busyOk) {
        EPD398_LOG("[EPD398] refresh BUSY timeout after %lums\n", elapsed);
        return false;
    }
    if (elapsed < EPD398_BUSY_EARLY_MS) {
        // BUSY 回来得太快，不可能是真实刷屏：把它等完。
        EPD398_LOG("[EPD398] BUSY only %lums -> fallback wait %lums\n",
                   elapsed, (unsigned long)EPD398_REFRESH_FALLBACK_MS);
        epd398WaitTick(EPD398_REFRESH_FALLBACK_MS);
    } else {
        epd398WaitTick(EPD398_REFRESH_SETTLE_MS);
    }
    return true;
}

// ---------------------------------------------------------------- 顶层
void epd398Display2bpp(const uint8_t *colorBuf) {
    if (!colorBuf) {
        EPD398_LOG("[EPD398] display skipped: null framebuffer\n");
        s_lastOperationOk = false;
        return;
    }
    ensureGpio();

    unsigned long t0 = millis();
    epd398Init();
    EPD398_LOG("[EPD398] init %lums\n", millis() - t0);

    epd398Upload2bpp(colorBuf);
    EPD398_LOG("[EPD398] data done %lums\n", millis() - t0);

    bool ok = epd398RefreshPanel();
    EPD398_LOG("[EPD398] refresh done %lums (ok=%d)\n", millis() - t0, ok ? 1 : 0);

    epd398PowerOff();
    EPD398_LOG("[EPD398] all done %lums\n", millis() - t0);

    s_lastOperationOk = ok;
}

bool epd398LastOperationOk() {
    return s_lastOperationOk;
}

// ============================================================================
//  帧缓冲辅助函数
// ============================================================================

uint8_t *epd398AllocBuffer() {
    uint8_t *buf = (uint8_t *)malloc(EPD398_COLOR_BYTES);
    if (buf) {
        EPD398_LOG("[EPD398] buffer %d bytes allocated\n", EPD398_COLOR_BYTES);
    } else {
        EPD398_LOG("[EPD398] buffer allocation FAILED (%d bytes)\n", EPD398_COLOR_BYTES);
    }
    return buf;
}

void epd398FreeBuffer(uint8_t *buf) {
    if (buf) {
        free(buf);
    }
}

// 2bpp 打包：每字节 4 个像素，最高位那对 = 最左边的像素。
void epd398Fill(uint8_t *buf, uint8_t color) {
    if (!buf) {
        return;
    }
    uint8_t packed = (uint8_t)((color & 0x03) * 0x55);   // 0b00/01/10/11 重复
    memset(buf, packed, EPD398_COLOR_BYTES);
}

void epd398SetPixel(uint8_t *buf, int x, int y, uint8_t color) {
    if (!buf || x < 0 || x >= EPD398_WIDTH || y < 0 || y >= EPD398_HEIGHT) {
        return;
    }
    int idx = y * EPD398_ROW_BYTES + (x >> 2);
    int shift = 6 - ((x & 0x03) * 2);
    buf[idx] = (uint8_t)((buf[idx] & ~(0x03 << shift)) | ((color & 0x03) << shift));
}

void epd398FillRect(uint8_t *buf, int x, int y, int w, int h, uint8_t color) {
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            epd398SetPixel(buf, i, j, color);
        }
    }
}

void epd398TestPattern(uint8_t *buf, const uint8_t *colors, int count) {
    if (!buf || !colors || count <= 0) {
        return;
    }
    epd398Fill(buf, EPD398_WHITE);

    int barW = EPD398_WIDTH / count;
    for (int c = 0; c < count; c++) {
        int x0 = c * barW;
        int x1 = (c == count - 1) ? EPD398_WIDTH : x0 + barW;
        epd398FillRect(buf, x0, 0, x1 - x0, EPD398_HEIGHT, colors[c]);
    }

    // 四角标记：每个角一个 24x24 的黑色方块，可以立刻暴露行映射错误或
        // 镜像/旋转的面板
    epd398FillRect(buf, 0, 0, 24, 24, EPD398_BLACK);
    epd398FillRect(buf, EPD398_WIDTH - 24, 0, 24, 24, EPD398_BLACK);
    epd398FillRect(buf, 0, EPD398_HEIGHT - 24, 24, 24, EPD398_BLACK);
    epd398FillRect(buf, EPD398_WIDTH - 24, EPD398_HEIGHT - 24, 24, 24, EPD398_BLACK);

        // 每 50 行一条水平标尺线
    for (int y = 50; y < EPD398_HEIGHT; y += 50) {
        epd398FillRect(buf, 0, y, EPD398_WIDTH, 1, EPD398_BLACK);
    }
}
