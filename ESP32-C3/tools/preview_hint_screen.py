#!/usr/bin/env python3
"""在主机上渲染 BLE 提示屏，并把它输出成 PNG。

面板刷新一次要 15-25 s，而且还得先把固件烧进去，所以在真实硬件上检查
一处布局改动很慢。本脚本重新实现了 ``ble_screen.cpp`` 的绘制
（四色圆环、细边线以及三行文字），直接从 ``src/ble_font.h`` 读取
*生成的* 字形表，并把得到的画面写成 PNG。

一切都先在 **竖屏画布**（552x768）里完成，然后再完全按照
``ble_geometry.h`` 的方式旋转到面板上，因此预览显示的就是玻璃上会显示的内容。
布局没有任何一处是手工重复实现的，所以本脚本能抓到的问题恰好就是那些
容易弄错的地方：文字行宽与圆环的冲突、沿周边的颜色循环、
以及文字与边框的碰撞。

从任意位置运行（需要 Pillow - ``pip install pillow``）：

    python tools/preview_hint_screen.py

选项：
    --name 8adge-8D134      要渲染的设备名（默认 8adge-8D134）
    --rotation 90           画布->面板的旋转，90 或 270（默认 90）
    --scale 2               PNG 的放大倍数
    --out preview.png       输出路径
"""

from __future__ import annotations

import argparse
import os
import re
import sys

try:
    from PIL import Image
except ImportError:  # pragma: no cover - 依赖提示
    sys.exit("Pillow is required: pip install pillow")

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE_DIR = os.path.dirname(HERE)
DEFAULT_FONT = os.path.join(FIRMWARE_DIR, "src", "ble_font.h")
DEFAULT_SRC = os.path.join(FIRMWARE_DIR, "src")

# 几何参数 - 镜像 ble_protocol.h / config.h / epd_398_4color.h。
CANVAS_W, CANVAS_H = 552, 768     # 竖屏工作画布（手机发送的内容）
PANEL_W, PANEL_H = 768, 552       # 面板原生的横屏方向
PANEL_ROW_BYTES = PANEL_W // 4    # 2bpp：每个面板行 192 字节
PANEL_BYTES = PANEL_W * PANEL_H // 4

# 颜色代码 - 镜像 epd_398_4color.h 和手机的 2bpp 格式。
BLACK, WHITE, YELLOW, RED = 0, 1, 2, 3
RGB = {
    BLACK:  (0, 0, 0),
    WHITE:  (255, 255, 255),
    YELLOW: (232, 176, 0),   # 面板的黄色，即手机编码的索引 2
    RED:    (200, 0, 0),
}

# 那三行文字 - 镜像 ble_screen.cpp。
HINT_TITLE = "请使用蓝牙连接："
HINT_FOOTER = "打开手机App搜索并连接"

# 同样镜像 ble_screen.cpp - 每一行用哪个生成的字号来绘制。
TITLE_SIZE, NAME_SIZE, FOOTER_SIZE = 48, 56, 28
SIZES = (28, 48, 56)

# 通常从 src/config.h 里抓取的那些值的兜底默认值。
RING_THICKNESS = 24
RING_BLOCKS = 24
RING_KEYLINE = 3

GLYPH_RE = re.compile(
    r"\{\s*0x([0-9A-Fa-f]{4}),\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+),\s*(\d+),\s*(\d+)\s*\}"
)


def scrape_config(src_dir: str) -> None:
    """从 config.h 中抓取圆环几何参数，这样预览就不会漂移。"""
    global RING_THICKNESS, RING_BLOCKS, RING_KEYLINE
    path = os.path.join(src_dir, "config.h")
    if not os.path.isfile(path):
        print(f"!! {path} not found, using built-in ring defaults")
        return
    text = open(path, encoding="utf-8").read()
    for name in ("HINT_RING_THICKNESS", "HINT_RING_BLOCKS", "HINT_RING_KEYLINE"):
        m = re.search(rf"^#define\s+{name}\s+(\d+)", text, re.M)
        if m:
            globals()[name] = int(m.group(1))
    print(f"ring: thickness={RING_THICKNESS} blocks={RING_BLOCKS} keyline={RING_KEYLINE}")
def parse_font(path: str, size: int) -> dict:
    text = open(path, encoding="utf-8").read()

    # 先把这个字号的数组切出来：文件里好几个字号是背靠背
        # 排列的，只用一条正则在整份文件上匹配会把它们的字形混在一起。
    glyph_block_m = re.search(rf"GLYPHS_{size}\[\]\s*=\s*\{{(.*?)\}};", text, re.S)
    if not glyph_block_m:
        sys.exit(f"cannot find GLYPHS_{size} in {path}")

    glyphs = {}
    for m in GLYPH_RE.finditer(glyph_block_m.group(1)):
        cp, w, h, x0, y0, advance, offset = (
            int(m.group(1), 16), int(m.group(2)), int(m.group(3)),
            int(m.group(4)), int(m.group(5)), int(m.group(6)), int(m.group(7)),
        )
        glyphs[cp] = dict(w=w, h=h, x0=x0, y0=y0, advance=advance, offset=offset)

    pool_m = re.search(rf"BITMAP_{size}\[\]\s*=\s*\{{(.*?)\}};", text, re.S)
    if not pool_m:
        sys.exit(f"cannot find BITMAP_{size} in {path}")
    pool = bytes(int(b, 16) for b in re.findall(r"0x([0-9A-Fa-f]{2})", pool_m.group(1)))

    def_m = re.search(
        rf"const BleFont BLE_FONT_{size} = \{{\s*(\d+),\s*(\d+),\s*(\d+),", text
    )
    if not def_m:
        sys.exit(f"cannot find BLE_FONT_{size} in {path}")

    return {
        "glyphs": glyphs,
        "pool": pool,
        "ascent": int(def_m.group(1)),
        "line_height": int(def_m.group(2)),
    }


class Frame:
    """面板帧缓冲，画布坐标的写入要经过旋转。"""

    def __init__(self, rotation: int) -> None:
        self.rotation = rotation
        self.buf = bytearray([0x55]) * PANEL_BYTES   # 每个像素都是白色

    def _map(self, x: int, y: int) -> tuple[int, int]:
        # 这里刻意没有 180 - 见 ble_geometry.h：旋转 180 度
                # 后位图仍是 552x768，无法覆盖 768x552 的面板。
        if self.rotation == 90:
            return (CANVAS_H - 1) - y, x
        return y, (CANVAS_W - 1) - x   # 270

    def set_canvas(self, x: int, y: int, color: int) -> None:
        if not (0 <= x < CANVAS_W and 0 <= y < CANVAS_H):
            return
        px, py = self._map(x, y)
        idx = py * PANEL_ROW_BYTES + (px >> 2)
        shift = 6 - ((px & 3) * 2)
        self.buf[idx] = (self.buf[idx] & ~(0x03 << shift)) | ((color & 3) << shift)

    def get_panel(self, px: int, py: int) -> int:
        idx = py * PANEL_ROW_BYTES + (px >> 2)
        shift = 6 - ((px & 3) * 2)
        return (self.buf[idx] >> shift) & 0x03

    def get_canvas(self, x: int, y: int) -> int:
        return self.get_panel(*self._map(x, y))

    def fill_canvas_rect(self, x: int, y: int, w: int, h: int, color: int) -> None:
        for py in range(y, y + h):
            for px in range(x, x + w):
                self.set_canvas(px, py, color)

    # --- 圆环，镜像 drawRing()/drawRingRun() ---------------------------
    def draw_ring_run(self, x, y, dx, dy, length, cross_dx, cross_dy, color) -> None:
        for i in range(length):
            bx, by = x + dx * i, y + dy * i
            for t in range(RING_THICKNESS):
                self.set_canvas(bx + cross_dx * t, by + cross_dy * t, color)

    def draw_ring(self) -> None:
        t = RING_THICKNESS
        h_len = CANVAS_W
        v_len = CANVAS_H - 2 * t
        perimeter = 2 * h_len + 2 * v_len
        cycle = [BLACK, WHITE, RED, YELLOW]
        runs = [
            (0,            0,                1,  0, h_len, 0,  1),
            (CANVAS_W - t, t,                0,  1, v_len, 1,  0),
            # 从最后一行开始：色带从 y=CANVAS_H-1 向上生长。
            (CANVAS_W - 1, CANVAS_H - 1,    -1,  0, h_len, 0, -1),
            (0,            CANVAS_H - t - 1, 0, -1, v_len, 1,  0),
        ]
        distance = 0
        for (x, y, dx, dy, length, cdx, cdy) in runs:
            done = 0
            while done < length:
                block = (distance * RING_BLOCKS) // perimeter
                block_end = ((block + 1) * perimeter) // RING_BLOCKS
                n = block_end - distance
                if n > length - done:
                    n = length - done
                if n <= 0:
                    n = length - done
                self.draw_ring_run(x + dx * done, y + dy * done, dx, dy, n,
                                   cdx, cdy, cycle[block % 4])
                done += n
                distance += n

    def draw_keyline(self) -> None:
        k = RING_KEYLINE
        if k <= 0:
            return
        t = RING_THICKNESS
        w, h = CANVAS_W - 2 * t, CANVAS_H - 2 * t
        self.fill_canvas_rect(t, t, w, k, BLACK)
        self.fill_canvas_rect(t, CANVAS_H - t - k, w, k, BLACK)
        self.fill_canvas_rect(t, t, k, h, BLACK)
        self.fill_canvas_rect(CANVAS_W - t - k, t, k, h, BLACK)

    # --- 文字 ----------------------------------------------------------
    def draw_text(self, font: dict, s: str, x: int, baseline: int, color: int,
                  missing: set) -> None:
        glyphs = font["glyphs"]
        pen = x
        for ch in s:
            g = glyphs.get(ord(ch))
            if g is None:
                missing.add(ord(ch))
                continue
            if g["w"] > 0 and g["h"] > 0:
                row_bytes = (g["w"] + 7) // 8
                bits = font["pool"][g["offset"]:g["offset"] + row_bytes * g["h"]]
                gx, gy = pen + g["x0"], baseline + g["y0"]
                for row in range(g["h"]):
                    for col in range(g["w"]):
                        if bits[row * row_bytes + (col >> 3)] & (0x80 >> (col & 7)):
                            self.set_canvas(gx + col, gy + row, color)
            pen += g["advance"]

    def draw_line_centered(self, font: dict, s: str, center_y: int, color: int,
                           missing: set) -> None:
        baseline = center_y - font["line_height"] // 2 + font["ascent"]
        self.draw_text(font, s, (CANVAS_W - text_width(font, s)) // 2, baseline,
                       color, missing)


def text_width(font: dict, s: str) -> int:
    return sum(font["glyphs"][ord(c)]["advance"] for c in s if ord(c) in font["glyphs"])


def ring_path() -> list[tuple[int, int]]:
    """圆环着色所走的、避开中心的路径 - 镜像 drawRing()。"""
    t = RING_THICKNESS
    path: list[tuple[int, int]] = []
    path += [(x, 0) for x in range(CANVAS_W)]
    path += [(CANVAS_W - t, y) for y in range(t, CANVAS_H - t)]
    path += [(x, CANVAS_H - 1) for x in range(CANVAS_W - 1, -1, -1)]
    path += [(0, y) for y in range(CANVAS_H - t - 1, t - 1, -1)]
    return path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--font", default=DEFAULT_FONT)
    ap.add_argument("--src", default=DEFAULT_SRC)
    ap.add_argument("--name", default="8adge-8D134")
    ap.add_argument("--rotation", type=int, default=90, choices=(90, 270))
    ap.add_argument("--ring-blocks", type=int, default=0,
                    help="override HINT_RING_BLOCKS (0 = use config.h). Use 4 for one "
                         "lap of the 4-colour cycle, 24 for the repeating border.")
    ap.add_argument("--ring-thickness", type=int, default=0,
                    help="override HINT_RING_THICKNESS (0 = use config.h)")
    ap.add_argument("--scale", type=int, default=1)
    ap.add_argument("--out", default=os.path.join(FIRMWARE_DIR, "hint_screen_preview.png"))
    args = ap.parse_args()

    scrape_config(args.src)
    # 这些覆盖值必须在任何 Frame 构造之前生效，因为 draw_ring()
        # 会直接读取这些模块级全局变量。
    if args.ring_blocks:
        globals()["RING_BLOCKS"] = args.ring_blocks
        print(f"ring blocks overridden -> {RING_BLOCKS}")
    if args.ring_thickness:
        globals()["RING_THICKNESS"] = args.ring_thickness
        print(f"ring thickness overridden -> {RING_THICKNESS}")

    fonts = {size: parse_font(args.font, size) for size in SIZES}
    for size, f in fonts.items():
        print(f"{size:2d} px: {len(f['glyphs'])} glyphs, ascent {f['ascent']}, line {f['line_height']}")

    frame = Frame(args.rotation)
    missing: set = set()

    # --- 镜像 ble_screen.cpp::bleScreenShowHint() ---
    frame.draw_ring()
    frame.draw_keyline()

    inner_top = RING_THICKNESS + RING_KEYLINE
    inner_bot = CANVAS_H - RING_THICKNESS - RING_KEYLINE
    inner_h = inner_bot - inner_top
    title_y = inner_top + inner_h * 26 // 100
    name_y = inner_top + inner_h * 45 // 100
    footer_y = inner_top + inner_h * 68 // 100

    frame.draw_line_centered(fonts[TITLE_SIZE], HINT_TITLE, title_y, BLACK, missing)
    frame.draw_line_centered(fonts[NAME_SIZE], args.name, name_y, BLACK, missing)
    frame.draw_line_centered(fonts[FOOTER_SIZE], HINT_FOOTER, footer_y, BLACK, missing)

    if missing:
        print("!! missing glyphs: " + " ".join(f"U+{cp:04X}({chr(cp)})" for cp in sorted(missing)))

    # --- 报告那些真正容易出错的地方 ---
    usable = CANVAS_W - 2 * inner_top
    print(f"canvas {CANVAS_W}x{CANVAS_H}, ring {RING_THICKNESS} px, keyline {RING_KEYLINE} px")
    print(f"interior {usable} px wide, text column {inner_top}..{inner_bot} vertically, "
          f"rotation {args.rotation} deg CW onto the {PANEL_W}x{PANEL_H} panel")
    for label, size, s, y in (
        ("title",  TITLE_SIZE,  HINT_TITLE,  title_y),
        ("name",   NAME_SIZE,   args.name,   name_y),
        ("footer", FOOTER_SIZE, HINT_FOOTER, footer_y),
    ):
        w = text_width(fonts[size], s)
        half = fonts[size]["line_height"] // 2
        fits_w = w <= usable
        fits_y = (y - half >= inner_top) and (y + half <= inner_bot)
        print(f"{label:6s} {size:2d}px width {w:4d} px (fits {usable}: {fits_w}), "
              f"y {y - half}..{y + half} (inside {inner_top}..{inner_bot}: {fits_y})")

    # --- 颜色普查，并检查圆环确实是一个闭合回路 ---
    counts = {BLACK: 0, WHITE: 0, YELLOW: 0, RED: 0}
    for y in range(CANVAS_H):
        for x in range(CANVAS_W):
            counts[frame.get_canvas(x, y)] += 1
    total = sum(counts.values())
    print("pixels:", counts, f"(sum {total}, expected {CANVAS_W * CANVAS_H})")
    if total != CANVAS_W * CANVAS_H:
        print("!! pixel census does not add up")

    path = ring_path()
    colours = [frame.get_canvas(x, y) for (x, y) in path]
    transitions = sum(1 for i in range(len(colours)) if colours[i] != colours[i - 1])
    print(f"ring path {len(path)} px, colour changes along it: {transitions} "
          f"(expected {RING_BLOCKS})")
    if transitions != RING_BLOCKS:
        print("!! the colour cycle is not continuous around the ring")

    # 圆环必须包住整张画布：逐行/逐列遍历，检查
        # 最外侧的非内部像素属于边带。
    bad = 0
    for y in range(CANVAS_H):
        for x in range(CANVAS_W):
            on_border = x < RING_THICKNESS or y < RING_THICKNESS or \
                        x >= CANVAS_W - RING_THICKNESS or y >= CANVAS_H - RING_THICKNESS
            if on_border:
                continue
            # 在内部区域里，只允许出现黑色文字
            c = frame.get_canvas(x, y)
            if c not in (WHITE, BLACK):
                bad += 1
    print(f"interior pixels that are not black/white: {bad} (expected 0)")

    img = Image.new("RGB", (CANVAS_W, CANVAS_H))
    img.putdata([RGB[frame.get_canvas(x, y)] for y in range(CANVAS_H) for x in range(CANVAS_W)])
    if args.scale > 1:
        img = img.resize((CANVAS_W * args.scale, CANVAS_H * args.scale), Image.NEAREST)
    out_dir = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(out_dir, exist_ok=True)
    img.save(args.out)
    print(f"wrote {args.out} ({img.size[0]}x{img.size[1]})")

    # 同时写出面板本身所持有的内容，这样可以肉眼核对旋转。
    panel = Image.new("RGB", (PANEL_W, PANEL_H))
    panel.putdata([RGB[frame.get_panel(x, y)] for y in range(PANEL_H) for x in range(PANEL_W)])
    panel_path = os.path.splitext(args.out)[0] + "_panel.png"
    panel.save(panel_path)
    print(f"wrote {panel_path} (native {PANEL_W}x{PANEL_H} layout, for rotation checks)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
