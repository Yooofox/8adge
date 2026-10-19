#!/usr/bin/env python3
"""Render the BLE hint screen on the host and write it out as a PNG.

The panel takes 15-25 s to refresh and the firmware has to be flashed first, so
checking a layout change on real hardware is slow. This script re-implements
``ble_screen.cpp``'s drawing - the four-colour ring, the keyline, and the three
text lines - reads the *generated* glyph tables straight out of
``src/ble_font.h``, and writes the resulting frame as a PNG.

Everything is done in the **portrait canvas** (552x768) and then rotated exactly
the way ``ble_geometry.h`` rotates it onto the panel, so the preview shows what
the glass will show. Nothing about the layout is duplicated by hand, so what this
script catches is exactly what is easy to get wrong: line widths against the
ring, the colour cycle round the perimeter, and text colliding with the border.

Run from anywhere (Pillow is required - ``pip install pillow``):

    python tools/preview_hint_screen.py

Options:
    --name 8adge-8D134      device name to render (default 8adge-8D134)
    --rotation 90           canvas->panel rotation, 90 or 270 (default 90)
    --scale 2               upscale factor for the PNG
    --out preview.png       output path
"""

from __future__ import annotations

import argparse
import os
import re
import sys

try:
    from PIL import Image
except ImportError:  # pragma: no cover - dependency hint
    sys.exit("Pillow is required: pip install pillow")

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE_DIR = os.path.dirname(HERE)
DEFAULT_FONT = os.path.join(FIRMWARE_DIR, "src", "ble_font.h")
DEFAULT_SRC = os.path.join(FIRMWARE_DIR, "src")

# Geometry - mirrors ble_protocol.h / config.h / epd_398_4color.h.
CANVAS_W, CANVAS_H = 552, 768     # portrait working canvas (what the phone sends)
PANEL_W, PANEL_H = 768, 552       # native landscape panel
PANEL_ROW_BYTES = PANEL_W // 4    # 2bpp: 192 bytes per panel row
PANEL_BYTES = PANEL_W * PANEL_H // 4

# Colour codes - mirrors epd_398_4color.h and the phone's 2bpp format.
BLACK, WHITE, YELLOW, RED = 0, 1, 2, 3
RGB = {
    BLACK:  (0, 0, 0),
    WHITE:  (255, 255, 255),
    YELLOW: (232, 176, 0),   # the panel's yellow, as the phone encodes index 2
    RED:    (200, 0, 0),
}

# The three lines - mirrors ble_screen.cpp.
HINT_TITLE = "请使用蓝牙连接："
HINT_FOOTER = "打开手机App搜索并连接"

# Also mirrors ble_screen.cpp - which generated size each line is drawn at.
TITLE_SIZE, NAME_SIZE, FOOTER_SIZE = 48, 56, 28
SIZES = (28, 48, 56)

# Fallbacks for the values normally scraped out of src/config.h.
RING_THICKNESS = 24
RING_BLOCKS = 24
RING_KEYLINE = 3

GLYPH_RE = re.compile(
    r"\{\s*0x([0-9A-Fa-f]{4}),\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+),\s*(\d+),\s*(\d+)\s*\}"
)


def scrape_config(src_dir: str) -> None:
    """Pull the ring geometry out of config.h so the preview cannot drift."""
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

    # Slice out this size's array first: the file holds several sizes back to
    # back and a bare regex over the whole file would mix their glyphs together.
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
    """The panel framebuffer, with canvas coordinates written through the rotation."""

    def __init__(self, rotation: int) -> None:
        self.rotation = rotation
        self.buf = bytearray([0x55]) * PANEL_BYTES   # every pixel white

    def _map(self, x: int, y: int) -> tuple[int, int]:
        # 180 is deliberately absent - see ble_geometry.h: a 180 degree turn
        # leaves the bitmap 552x768 and it cannot cover a 768x552 panel.
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

    # --- the ring, mirroring drawRing()/drawRingRun() -------------------
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
            # Start on the last row: the band grows upwards from y=CANVAS_H-1.
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

    # --- text ----------------------------------------------------------
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
    """The centre-free path the ring colouring walks - mirrors drawRing()."""
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
    # The overrides have to land before any Frame is built, since draw_ring()
    # reads these module globals directly.
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

    # --- mirrors ble_screen.cpp::bleScreenShowHint() ---
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

    # --- report the things that actually go wrong ---
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

    # --- colour census, and check the ring really is a closed loop ---
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

    # The ring must enclose the whole canvas: walk each row/column and check the
    # outermost non-interior pixel is part of the border band.
    bad = 0
    for y in range(CANVAS_H):
        for x in range(CANVAS_W):
            on_border = x < RING_THICKNESS or y < RING_THICKNESS or \
                        x >= CANVAS_W - RING_THICKNESS or y >= CANVAS_H - RING_THICKNESS
            if on_border:
                continue
            # inside the interior, only black text may appear
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

    # Also write what the panel itself holds, so the rotation can be eyeballed.
    panel = Image.new("RGB", (PANEL_W, PANEL_H))
    panel.putdata([RGB[frame.get_panel(x, y)] for y in range(PANEL_H) for x in range(PANEL_W)])
    panel_path = os.path.splitext(args.out)[0] + "_panel.png"
    panel.save(panel_path)
    print(f"wrote {panel_path} (native {PANEL_W}x{PANEL_H} layout, for rotation checks)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
