// ============================================================================
//  BLE 提示屏 - 它是什么、为何存在，见 ble_screen.h。
//
//  这里的一切都用**画布**坐标（552 x 768 竖屏）绘制，并经过
//  bleSetCanvasPixel() 旋转到面板自己的 768 x 552 帧缓冲上。下面没有任何代码
//  需要知道旋转的存在，而修改 BLE_ROTATION 会让边框和文字一起移动。
//
//  颜色码来自 epd_398_4color.h：0x00 黑、0x01 白、0x02 黄、0x03 红。它们就是
//  手机在 2bpp 图像里发送的同一套码，所以面板和 App 在构造上就是一致的。
// ============================================================================

#include "ble_screen.h"

#include <Arduino.h>
#include <string.h>

#include "ble_font.h"
#include "ble_geometry.h"
#include "ble_protocol.h"
#include "config.h"
#include "epd_398_4color.h"

// ---------------------------------------------------------------- 几何

static const int CW = BLE_IMAGE_WIDTH;    // 552
static const int CH = BLE_IMAGE_HEIGHT;   // 768

// ---------------------------------------------------------------- 辅助函数

static void fillRectCanvas(uint8_t *buf, int x, int y, int w, int h, uint8_t color) {
    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) {
            bleSetCanvasPixel(buf, px, py, color);
        }
    }
}

// 解码一个 UTF-8 标量值。返回该标量并让 *i 越过它。
// 畸形输入产出 U+FFFD 并只前进一个字节，因此循环总能推进，杂散字节永远不会
// 卡死这一屏。
static uint32_t utf8Next(const char *s, int len, int *i) {
    const uint8_t b0 = (uint8_t)s[*i];
    if (b0 < 0x80) { (*i)++; return b0; }

    int extra;
    uint32_t cp;
    if ((b0 & 0xE0) == 0xC0) { extra = 1; cp = b0 & 0x1F; }
    else if ((b0 & 0xF0) == 0xE0) { extra = 2; cp = b0 & 0x0F; }
    else if ((b0 & 0xF8) == 0xF0) { extra = 3; cp = b0 & 0x07; }
    else { (*i)++; return 0xFFFD; }

    if (*i + extra >= len) { (*i)++; return 0xFFFD; }
    for (int k = 1; k <= extra; k++) {
        const uint8_t bk = (uint8_t)s[*i + k];
        if ((bk & 0xC0) != 0x80) { (*i)++; return 0xFFFD; }
        cp = (cp << 6) | (bk & 0x3F);
    }
    *i += extra + 1;
    return cp;
}

// 对按码点排好序的字形表做二分查找。
static const BleGlyph *findGlyph(const BleFont &font, uint32_t cp) {
    int lo = 0;
    int hi = (int)font.glyphCount - 1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        const uint32_t at = font.glyphs[mid].codepoint;
        if (at == cp) return &font.glyphs[mid];
        if (at < cp) lo = mid + 1; else hi = mid - 1;
    }
    return nullptr;
}

// 绘制一个 UTF-8 字符串，笔起点在 `x`，基线在 `baseline`。
static void drawTextCJKPx(uint8_t *buf, const BleFont &font, const char *s,
                          int x, int baseline, uint8_t color) {
    const int len = (int)strlen(s);
    int i = 0;
    int penX = x;
    while (i < len) {
        const uint32_t cp = utf8Next(s, len, &i);
        const BleGlyph *g = findGlyph(font, cp);
        if (!g) continue;
        if (g->w > 0 && g->h > 0) {
            const int rowBytes = (g->w + 7) / 8;
            const uint8_t *bits = font.bitmap + g->offset;
            const int gx = penX + g->x0;
            const int gy = baseline + g->y0;
            for (int row = 0; row < g->h; row++) {
                for (int col = 0; col < g->w; col++) {
                    if ((bits[row * rowBytes + (col >> 3)] & (0x80 >> (col & 7))) == 0) continue;
                    bleSetCanvasPixel(buf, gx + col, gy + row, color);
                }
            }
        }
        penX += g->advance;
    }
}

static int textWidth(const BleFont &font, const char *s) {
    const int len = (int)strlen(s);
    int i = 0;
    int width = 0;
    while (i < len) {
        const uint32_t cp = utf8Next(s, len, &i);
        const BleGlyph *g = findGlyph(font, cp);
        if (g) width += g->advance;
    }
    return width;
}

// 在画布上水平居中，并让*行框*在 `centerY` 上居中。让行框而非字形墨迹居中，
// 正是让这三行保持稳定节奏的原因，尽管它们的字形高度差别很大。
static void drawLineCenteredAtY(uint8_t *buf, const BleFont &font, const char *s,
                                int centerY, uint8_t color) {
    const int baseline = centerY - (int)font.lineHeight / 2 + (int)font.ascent;
    drawTextCJKPx(buf, font, s, (CW - textWidth(font, s)) / 2, baseline, color);
}

// ---------------------------------------------------------------- 边框

// 环的四种颜色，按用户要求的顺序排列。
static const uint8_t RING_CYCLE[4] = {
    EPD398_BLACK, EPD398_WHITE, EPD398_RED, EPD398_YELLOW,
};

// 环的一段直线：从 (x, y) 起以 (dx, dy) 步进 `len` 个像素，带宽沿
// (crossDx, crossDy) 方向深 `thickness` 个像素。
static void drawRingRun(uint8_t *buf, int x, int y, int dx, int dy, int len,
                        int crossDx, int crossDy, int thickness, uint8_t color) {
    for (int i = 0; i < len; i++) {
        const int bx = x + dx * i;
        const int by = y + dy * i;
        for (int t = 0; t < thickness; t++) {
            bleSetCanvasPixel(buf, bx + crossDx * t, by + crossDy * t, color);
        }
    }
}

// 把环当作一个连续的环行走，逐块上色。
//
// 路径从画布左上角出发顺时针前进：越过顶部、沿右侧向下、沿底部返回、沿左侧向上。
// 两条水平段横跨整个宽度，所以它们占据四个角；垂直段只覆盖它们之间剩下的部分，
// 这正是让这个环既没有缺口、也没有重复上色之角的原因。
//
// 块索引来自*路径距离*，而不是每一侧各自的计数器，所以一个块可以跨过拐角，
// 颜色循环依然连续不断。
static void drawRing(uint8_t *buf) {
    const int T = HINT_RING_THICKNESS;
    const int hLen = CW;             // 水平段：整个宽度，含四个角
    const int vLen = CH - 2 * T;     // 垂直段：只覆盖二者之间的部分
    const int perimeter = 2 * hLen + 2 * vLen;

    struct Run {
        int x, y;             // 起点
        int dx, dy;           // 沿路径的步进
        int len;              // 沿路径的像素数
        int crossDx, crossDy; // 横跨色带的步进
    };
    const Run runs[4] = {
        // 顶部：左   -> 右，色带向下生长（y 0..T-1）
        { 0,      0,          1,  0, hLen, 0,  1 },
        // 右侧：上    -> 下，色带向左生长（x CW-T..CW-1）
        { CW - T, T,          0,  1, vLen, 1,  0 },
        // 底部：右  -> 左，色带向上生长（y CH-T..CH-1）
        // 注意：起点是*最后一行*，不是 CH-T - 让色带朝另一个方向生长会把它向上
        // 走进内部，而不是向下走进底边，从而在文字区域画出一道杂散条纹。
        { CW - 1, CH - 1,    -1,  0, hLen, 0, -1 },
        // 左侧：  底 -> 顶，色带向右生长（x 0..T-1）
        { 0,      CH - T - 1, 0, -1, vLen, 1,  0 },
    };

    int distance = 0;
    for (int r = 0; r < 4; r++) {
        const Run &run = runs[r];
        int done = 0;
        while (done < run.len) {
            const int block = (distance * HINT_RING_BLOCKS) / perimeter;
            const int blockEnd = ((block + 1) * perimeter) / HINT_RING_BLOCKS;
            int len = blockEnd - distance;
            if (len > run.len - done) len = run.len - done;
            if (len <= 0) len = run.len - done;   // 只在正好落在块边界时发生

            drawRingRun(buf, run.x + run.dx * done, run.y + run.dy * done,
                        run.dx, run.dy, len, run.crossDx, run.crossDy, T,
                        RING_CYCLE[block % 4]);

            done += len;
            distance += len;
        }
    }
}

// 紧贴环内侧的一条细线。没有它，环上的白色块会直接贴着白色内部，边框看起来
// 就像有缺口。
static void drawRingKeyline(uint8_t *buf) {
    const int K = HINT_RING_KEYLINE;
    if (K <= 0) return;
    const int T = HINT_RING_THICKNESS;
    const int w = CW - 2 * T;
    const int h = CH - 2 * T;
    fillRectCanvas(buf, T, T, w, K, EPD398_BLACK);              // 上
    fillRectCanvas(buf, T, CH - T - K, w, K, EPD398_BLACK);     // 下
    fillRectCanvas(buf, T, T, K, h, EPD398_BLACK);              // 左
    fillRectCanvas(buf, CW - T - K, T, K, h, EPD398_BLACK);     // 右
}

// ---------------------------------------------------------------- 屏幕

// 这三行。第一行和第三行要与 tools/gen_ble_font.py 里的 BLE_STRINGS 保持一致 -
// 生成的子集里没有的字形会被静默地渲染成空白。
static const char *HINT_TITLE  = "请使用蓝牙连接：";
static const char *HINT_FOOTER = "打开手机App搜索并连接";

bool bleScreenShowHint(uint8_t *buf, const char *deviceName) {
    if (!buf) {
        BLE_LOG("[SCREEN] no framebuffer, hint screen skipped\n");
        return false;
    }

    // 0x55 = 0b01010101 -> 每个像素都是白色（颜色码 1，重复四次）。
        // 纯色填充与旋转无关，所以内部只需一次 memset。
    memset(buf, 0x55, EPD398_COLOR_BYTES);

    drawRing(buf);
    drawRingKeyline(buf);

        // 文字位于环及其细线让出的空间里，分布在该区域的竖直三等分处。设备名 -
        // 用户唯一必须读出来并核对的东西 - 是三行中最大的那一行。
    const int innerTop = HINT_RING_THICKNESS + HINT_RING_KEYLINE;
    const int innerBot = CH - HINT_RING_THICKNESS - HINT_RING_KEYLINE;
    const int innerH = innerBot - innerTop;

    const int titleY  = innerTop + innerH * 26 / 100;
    const int nameY   = innerTop + innerH * 45 / 100;
    const int footerY = innerTop + innerH * 68 / 100;

    drawLineCenteredAtY(buf, BLE_FONT_48, HINT_TITLE,  titleY,  EPD398_BLACK);
    drawLineCenteredAtY(buf, BLE_FONT_56, deviceName,  nameY,   EPD398_BLACK);
    drawLineCenteredAtY(buf, BLE_FONT_28, HINT_FOOTER, footerY, EPD398_BLACK);

    epd398Display2bpp(buf);
    const bool ok = epd398LastOperationOk();

    BLE_LOG("[SCREEN] hint shown (%s): %s\n", ok ? "ok" : "panel timeout", deviceName);
    return ok;
}
