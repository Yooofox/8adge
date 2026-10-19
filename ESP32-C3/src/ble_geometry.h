#ifndef BADGE8_BLE_GEOMETRY_H
#define BADGE8_BLE_GEOMETRY_H

// ============================================================================
//  Portrait canvas -> panel rotation.
//
//  The panel is a 768x552 *landscape* part, but this device is meant to live in
//  a portrait photo frame (phones, cameras, picture frames - all portrait). So
//  everything in this firmware is expressed in one portrait **canvas** of
//  552 x 768 and rotated onto the panel as the last step:
//
//    * the phone packs its image in canvas coordinates (BLE_IMAGE_WIDTH x
//      BLE_IMAGE_HEIGHT, see ble_protocol.h) and never has to know which way
//      round the panel's gate driver happens to be wired;
//    * the hint screen draws its border and text in canvas coordinates too.
//
//  Both go through bleSetCanvasPixel(), so one define (BLE_ROTATION in
//  config.h) flips the whole device, and the on-panel relationship between the
//  hint screen and a phone image can never drift apart.
//
//  The rotation is exact: 552 * 768 == 768 * 552, so the canvas and the panel
//  hold the same number of pixels and no scaling or cropping is involved.
// ============================================================================

#include <stdint.h>

#include "ble_protocol.h"
#include "config.h"
#include "epd_398_4color.h"

#if BLE_ROTATION != 90 && BLE_ROTATION != 270
// 180 is deliberately NOT offered: rotating a 552x768 canvas by 180 degrees
// yields another 552x768 bitmap, which cannot be laid over a 768x552 panel -
// the aspect would have to flip, and the mapping would run off the end of the
// framebuffer. Portrait on this glass means +/-90 degrees, and 90 vs 270 is
// exactly the "upright or upside-down" choice.
#error "BLE_ROTATION must be 90 or 270 (180 cannot map a portrait canvas onto this landscape panel)"
#endif

// The rotation is a pure permutation, so the canvas and the panel must hold the
// same number of pixels - and every payload length, block count, retry length
// and CRC in the protocol is derived from the canvas. Checking it here turns a
// comment into something the compiler enforces: a stray edit to either geometry
// would otherwise surface only as a transfer that always fails END_IMAGE.
static_assert(EPD398_WIDTH * EPD398_HEIGHT == BLE_IMAGE_WIDTH * BLE_IMAGE_HEIGHT,
              "canvas and panel must hold the same number of pixels (rotation is a permutation)");
static_assert(EPD398_COLOR_BYTES == (BLE_IMAGE_WIDTH * BLE_IMAGE_HEIGHT) / 4,
              "panel framebuffer must equal the canvas 2bpp payload length");
static_assert(EPD398_WIDTH == BLE_IMAGE_HEIGHT && EPD398_HEIGHT == BLE_IMAGE_WIDTH,
              "with a +/-90 rotation the panel must be the canvas turned on its side");

// Map a canvas coordinate onto the panel's own framebuffer coordinate system.
//
// The angle is measured clockwise: BLE_ROTATION = 90 means the canvas' top-left
// corner ends up at the panel's top-right corner; 270 puts it at the panel's
// bottom-left. Checked with tools/check_rotation.py that both are bijections and
// that every panel pixel is written exactly once.
static inline void bleMapCanvasToPanel(int x, int y, int *outPx, int *outPy) {
#if BLE_ROTATION == 90
    *outPx = (BLE_IMAGE_HEIGHT - 1) - y;
    *outPy = x;
#else  // 270
    *outPx = y;
    *outPy = (BLE_IMAGE_WIDTH - 1) - x;
#endif
}

// One pixel of the panel framebuffer, in *panel* coordinates.
// Only valid bounds-wise after bleMapCanvasToPanel(); callers pass canvas
// coordinates and let the mapping keep them in range, but the checks below make
// a future 4-rotation switch safe.
static inline void bleSetPanelPixel(uint8_t *buf, int px, int py, uint8_t color) {
    if (!buf) return;
    if (px < 0 || px >= EPD398_WIDTH || py < 0 || py >= EPD398_HEIGHT) return;
    const int idx = py * EPD398_ROW_BYTES + (px >> 2);
    const int shift = 6 - ((px & 0x03) * 2);
    buf[idx] = (uint8_t)((buf[idx] & ~(0x03 << shift)) | ((color & 0x03) << shift));
}

// One pixel of the canvas, written through the rotation.
static inline void bleSetCanvasPixel(uint8_t *buf, int x, int y, uint8_t color) {
    if (x < 0 || x >= BLE_IMAGE_WIDTH || y < 0 || y >= BLE_IMAGE_HEIGHT) return;
    int px = 0, py = 0;
    bleMapCanvasToPanel(x, y, &px, &py);
    bleSetPanelPixel(buf, px, py, color);
}

// Read one canvas pixel back out of the framebuffer, through the same rotation.
//
// This is what makes the whole-image CRC verifiable *after* a repair: the device
// no longer has to know the payload it received, it can reconstruct it from the
// pixels it ended up with. Going through the same mapping in the other direction
// is also what guarantees the two never disagree.
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
