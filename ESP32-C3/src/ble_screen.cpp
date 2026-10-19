// ============================================================================
//  The BLE hint screen - see ble_screen.h for what it is and why it exists.
//
//  Everything here is drawn in **canvas** coordinates (552 x 768 portrait) and
//  goes through bleSetCanvasPixel(), which rotates it onto the panel's own
//  768 x 552 framebuffer. Nothing below has to know about the rotation, and a
//  change to BLE_ROTATION moves the border and the text together.
//
//  Colour codes come from epd_398_4color.h: 0x00 black, 0x01 white, 0x02 yellow,
//  0x03 red. They are the same codes the phone sends in a 2bpp image, so the
//  panel and the App agree by construction.
// ============================================================================

#include "ble_screen.h"

#include <Arduino.h>
#include <string.h>

#include "ble_font.h"
#include "ble_geometry.h"
#include "ble_protocol.h"
#include "config.h"
#include "epd_398_4color.h"

// ---------------------------------------------------------------- geometry

static const int CW = BLE_IMAGE_WIDTH;    // 552
static const int CH = BLE_IMAGE_HEIGHT;   // 768

// ---------------------------------------------------------------- helpers

static void fillRectCanvas(uint8_t *buf, int x, int y, int w, int h, uint8_t color) {
    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) {
            bleSetCanvasPixel(buf, px, py, color);
        }
    }
}

// Decode one UTF-8 scalar value. Returns the scalar and advances *i past it.
// Malformed input yields U+FFFD and a one-byte advance, so the loop always makes
// progress and a stray byte can never hang the screen.
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

// Binary search the codepoint-sorted glyph table.
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

// Draw a UTF-8 string with the pen origin at `x` and the baseline at `baseline`.
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

// Horizontally centred on the canvas, with the *line box* centred on `centerY`.
// Centring the line box rather than the ink is what keeps the three lines on a
// stable rhythm even though their glyphs have very different heights.
static void drawLineCenteredAtY(uint8_t *buf, const BleFont &font, const char *s,
                                int centerY, uint8_t color) {
    const int baseline = centerY - (int)font.lineHeight / 2 + (int)font.ascent;
    drawTextCJKPx(buf, font, s, (CW - textWidth(font, s)) / 2, baseline, color);
}

// ---------------------------------------------------------------- border

// The four colours of the ring, in the order the user asked for.
static const uint8_t RING_CYCLE[4] = {
    EPD398_BLACK, EPD398_WHITE, EPD398_RED, EPD398_YELLOW,
};

// A straight run of the ring: `len` pixels starting at (x, y) stepping by
// (dx, dy), with the band `thickness` pixels deep along (crossDx, crossDy).
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

// Walk the ring as one continuous loop and colour it block by block.
//
// The path starts at the canvas' top-left corner and goes clockwise: across the
// top, down the right side, back along the bottom, up the left side. The two
// horizontal runs span the full width so they own the corners; the vertical runs
// only cover what is left between them, which is what keeps the loop free of
// both gaps and double-painted corners.
//
// The block index comes from the *path distance*, not from a per-side counter,
// so a block may straddle a corner and the colour cycle stays unbroken.
static void drawRing(uint8_t *buf) {
    const int T = HINT_RING_THICKNESS;
    const int hLen = CW;             // horizontal runs: full width, corners included
    const int vLen = CH - 2 * T;     // vertical runs: whatever is between them
    const int perimeter = 2 * hLen + 2 * vLen;

    struct Run {
        int x, y;             // start
        int dx, dy;           // step along the path
        int len;              // pixels along the path
        int crossDx, crossDy; // step across the band
    };
    const Run runs[4] = {
        // top:    left   -> right,  band grows downwards (y 0..T-1)
        { 0,      0,          1,  0, hLen, 0,  1 },
        // right:  top    -> bottom, band grows leftwards  (x CW-T..CW-1)
        { CW - T, T,          0,  1, vLen, 1,  0 },
        // bottom: right  -> left,   band grows upwards   (y CH-T..CH-1)
        // NOTE: the start is the *last* row, not CH-T - growing the band the
        // other way would walk it up into the interior instead of down into the
        // bottom edge, painting a stray stripe across the text area.
        { CW - 1, CH - 1,    -1,  0, hLen, 0, -1 },
        // left:   bottom -> top,    band grows rightwards (x 0..T-1)
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
            if (len <= 0) len = run.len - done;   // only on an exact block boundary

            drawRingRun(buf, run.x + run.dx * done, run.y + run.dy * done,
                        run.dx, run.dy, len, run.crossDx, run.crossDy, T,
                        RING_CYCLE[block % 4]);

            done += len;
            distance += len;
        }
    }
}

// A hairline just inside the ring. Without it a white block of the ring sits
// directly against the white interior and the border reads as if it had gaps.
static void drawRingKeyline(uint8_t *buf) {
    const int K = HINT_RING_KEYLINE;
    if (K <= 0) return;
    const int T = HINT_RING_THICKNESS;
    const int w = CW - 2 * T;
    const int h = CH - 2 * T;
    fillRectCanvas(buf, T, T, w, K, EPD398_BLACK);              // top
    fillRectCanvas(buf, T, CH - T - K, w, K, EPD398_BLACK);     // bottom
    fillRectCanvas(buf, T, T, K, h, EPD398_BLACK);              // left
    fillRectCanvas(buf, CW - T - K, T, K, h, EPD398_BLACK);     // right
}

// ---------------------------------------------------------------- screen

// The three lines. Keep the first and the third in sync with
// tools/gen_ble_font.py's BLE_STRINGS - a character that is not in the generated
// subset renders as a gap, silently.
static const char *HINT_TITLE  = "请使用蓝牙连接：";
static const char *HINT_FOOTER = "打开手机App搜索并连接";

bool bleScreenShowHint(uint8_t *buf, const char *deviceName) {
    if (!buf) {
        BLE_LOG("[SCREEN] no framebuffer, hint screen skipped\n");
        return false;
    }

    // 0x55 = 0b01010101 -> every pixel WHITE (colour code 1, four times over).
    // A solid fill is rotation-invariant, so the interior is one memset.
    memset(buf, 0x55, EPD398_COLOR_BYTES);

    drawRing(buf);
    drawRingKeyline(buf);

    // The text sits in what the ring and its keyline leave free, spread over the
    // vertical thirds of that area. The device name - the one thing the user has
    // to read and match - is the largest of the three lines.
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
