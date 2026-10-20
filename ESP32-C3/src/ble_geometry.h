#ifndef BADGE8_BLE_GEOMETRY_H
#define BADGE8_BLE_GEOMETRY_H

// ============================================================================
//  竖版画布 -> 面板旋转。
//
//  面板是 768x552 的 *横版* 器件，但本设备要装在竖版相框里（手机、相机、
//  相框 - 全是竖版）。因此本固件中的所有内容都用一块 552 x 768 的竖版 **画布**
//  来表达，并在最后一步旋转到面板上：
//
//    * 手机按画布坐标打包它的图像（BLE_IMAGE_WIDTH x
//      BLE_IMAGE_HEIGHT，见 ble_protocol.h），完全不必知道面板的栅极驱动器
//      究竟是怎么接线的；
//    * 提示屏也按画布坐标绘制自己的边框和文字。
//
//  两者都经过 bleSetCanvasPixel()，因此一个宏（config.h 中的
//  BLE_ROTATION）就能翻转整个设备，而提示屏与手机图像在面板上的相对关系
//  永远不会错位。
//
//  这个旋转是精确的：552 * 768 == 768 * 552，因此画布与面板持有的像素数
//  完全一样，不涉及任何缩放或裁剪。
// ============================================================================

#include <stdint.h>

#include "ble_protocol.h"
#include "config.h"
#include "epd_398_4color.h"

#if BLE_ROTATION != 90 && BLE_ROTATION != 270
// 刻意不提供 180：把 552x768 的画布旋转 180 度得到的仍是 552x768 的位图，
// 它无法铺到 768x552 的面板上 - 宽高比必须翻转，映射会跑出帧缓冲的末尾。
// 在这块玻璃上竖版就意味着 +/-90 度，而 90 与 270 恰好就是
// "正立还是倒立"这个选择。
#error "BLE_ROTATION must be 90 or 270 (180 cannot map a portrait canvas onto this landscape panel)"
#endif

// 旋转是一个纯粹的置换，因此画布与面板必须持有相同数量的像素 - 而且协议里
// 每个载荷长度、块数量、重传长度和 CRC 都源自画布。在这里检查它，就把一条
// 注释变成了编译器强制执行的东西：否则对任一处几何参数的一笔误改，
// 只会表现为一次永远在 END_IMAGE 失败的传输。
static_assert(EPD398_WIDTH * EPD398_HEIGHT == BLE_IMAGE_WIDTH * BLE_IMAGE_HEIGHT,
              "canvas and panel must hold the same number of pixels (rotation is a permutation)");
static_assert(EPD398_COLOR_BYTES == (BLE_IMAGE_WIDTH * BLE_IMAGE_HEIGHT) / 4,
              "panel framebuffer must equal the canvas 2bpp payload length");
static_assert(EPD398_WIDTH == BLE_IMAGE_HEIGHT && EPD398_HEIGHT == BLE_IMAGE_WIDTH,
              "with a +/-90 rotation the panel must be the canvas turned on its side");

// 协议的块几何有两处上限，改一个常量就很容易破坏，而两种失败在链路上都是
// 静默的：
//   * 可接受的最小块尺寸所需的块数，不能超过 END_IMAGE 失败位图
//     （BLE_MAX_BLOCKS）能容纳的数量；
//   * BLOCK_CRCS 帧每块携带 4 字节，因此即使是最小的块尺寸，也必须仍然装得进
//     帧格式里那个单字节长度。
// 这样就把例如"不碰 BLE_MAX_BLOCKS 而抬高画布或降低
// BLE_BLOCK_SHIFT_MIN"排除掉了。
static_assert((EPD398_COLOR_BYTES + (1 << BLE_BLOCK_SHIFT_MIN) - 1) / (1 << BLE_BLOCK_SHIFT_MIN) <= BLE_MAX_BLOCKS,
              "the smallest block size would need more blocks than BLE_MAX_BLOCKS holds");
static_assert(BLE_FRAME_HEADER_LEN + 4 + 4 * ((EPD398_COLOR_BYTES + (1 << BLE_BLOCK_SHIFT_MIN) - 1) / (1 << BLE_BLOCK_SHIFT_MIN)) <= BLE_FRAME_MAX_LEN,
              "a BLOCK_CRCS frame for the smallest block size would not fit the frame format");

// 把画布坐标映射到面板自己的帧缓冲坐标系。
//
// 角度按顺时针度量：BLE_ROTATION = 90 表示画布的左上角最终落在面板的右上角；
// 270 则把它放在面板的左下角。已用 tools/check_rotation.py 检查过两者都是
// 双射，且每个面板像素都恰好被写入一次。
static inline void bleMapCanvasToPanel(int x, int y, int *outPx, int *outPy) {
#if BLE_ROTATION == 90
    *outPx = (BLE_IMAGE_HEIGHT - 1) - y;
    *outPy = x;
#else  // 270
    *outPx = y;
    *outPy = (BLE_IMAGE_WIDTH - 1) - x;
#endif
}

// 面板帧缓冲的一个像素，使用 *面板* 坐标。
// 只有在经过 bleMapCanvasToPanel() 之后边界才是有效的；调用方传入画布坐标
// 并让映射把它们保持在范围内，但下面的检查让将来切换到 4 种旋转也是安全的。
static inline void bleSetPanelPixel(uint8_t *buf, int px, int py, uint8_t color) {
    if (!buf) return;
    if (px < 0 || px >= EPD398_WIDTH || py < 0 || py >= EPD398_HEIGHT) return;
    const int idx = py * EPD398_ROW_BYTES + (px >> 2);
    const int shift = 6 - ((px & 0x03) * 2);
    buf[idx] = (uint8_t)((buf[idx] & ~(0x03 << shift)) | ((color & 0x03) << shift));
}

// 画布的一个像素，经由旋转写入。
static inline void bleSetCanvasPixel(uint8_t *buf, int x, int y, uint8_t color) {
    if (x < 0 || x >= BLE_IMAGE_WIDTH || y < 0 || y >= BLE_IMAGE_HEIGHT) return;
    int px = 0, py = 0;
    bleMapCanvasToPanel(x, y, &px, &py);
    bleSetPanelPixel(buf, px, py, color);
}

// 通过同一个旋转，从帧缓冲里读回一个画布像素。
//
// 正是这一点让整幅图像 CRC 在 *一次修复之后* 仍然可验证：设备不再需要知道
// 自己收到的载荷，它可以从最终得到的那些像素把它重建出来。反方向也走同一套
// 映射，同样也是保证二者永远不会互相矛盾的原因。
static inline uint8_t bleGetCanvasPixel(const uint8_t *buf, int x, int y) {
    if (!buf) return EPD398_WHITE;
    if (x < 0 || x >= BLE_IMAGE_WIDTH || y < 0 || y >= BLE_IMAGE_HEIGHT) return EPD398_WHITE;
    int px = 0, py = 0;
    bleMapCanvasToPanel(x, y, &px, &py);
    const int idx = py * EPD398_ROW_BYTES + (px >> 2);
    const int shift = 6 - ((px & 0x03) * 2);
    return (uint8_t)((buf[idx] >> shift) & 0x03);
}

#endif // BADGE8_BLE_GEOMETRY_H
