#!/usr/bin/env python3
"""Generate the subset bitmap font used by the BLE hint screen.

The BLE firmware has no WiFi and therefore no captive portal, but it still has
to put one Chinese sentence on the panel:

    请使用蓝牙连接：
    8adge-XXXXX
    打开手机App搜索并连接

on its portrait 552x768 canvas - see `ble_screen.cpp`. The firmware's built-in
text path (`drawText()`) is a hand-written 5x7 ASCII font, so Chinese needs a
rasterised subset. Rather than embedding a whole CJK face, this script rasterises
**only the characters those three lines use** plus full printable ASCII (the
device name is built at runtime from the MAC address, so every hex digit must
render) and emits a flat, binary-searchable glyph table.

The source face ships with this project, so nothing outside the project folder
is needed to rebuild the font:

    assets/fonts/HarmonyOS_Sans_SC_Bold.ttf   (HarmonyOS Sans Fonts License)

Use the SC sub-family, not the base ``HarmonyOS_Sans`` faces: those have 874
codepoints and **no Chinese at all**, which would render every label as blank.

Run from anywhere (Pillow is required - ``pip install pillow``):

    python tools/gen_ble_font.py

It rewrites ``src/ble_font.h``. Re-run whenever BLE_STRINGS or SIZES change.
"""

from __future__ import annotations

import argparse
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:  # pragma: no cover - dependency hint
    sys.exit("Pillow is required: pip install pillow")

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE_DIR = os.path.dirname(HERE)
FONT_DIR = os.path.join(FIRMWARE_DIR, "assets", "fonts")

DEFAULT_TTF = os.path.join(FONT_DIR, "HarmonyOS_Sans_SC_Bold.ttf")
DEFAULT_OUT = os.path.join(FIRMWARE_DIR, "src", "ble_font.h")

# ---------------------------------------------------------------- charset
# Everything the hint screen draws. Keep in sync with ble_screen.cpp.
# The fullwidth colon U+FF1A is used deliberately: it is what the screen text
# asks for, and it is not the same glyph as ASCII ':'.
BLE_STRINGS = [
    "请使用蓝牙连接：",
    "8adge-XXXXX",
    "打开手机App搜索并连接",
]

# Printable ASCII, so a runtime-built device name ("8adge-A1B2C") always
# renders.
ASCII_RANGE = range(0x20, 0x7F)

# ---------------------------------------------------------------- sizes
# Pixel sizes, chosen against the 552x768 portrait canvas laid out in
# ble_screen.cpp (ring 24 px + 3 px keyline leaves 498 px of usable width).
# Measured widths at these sizes (see the run-time report):
#   28 px  footer      "打开手机App搜索并连接"  ~308 px
#   48 px  title       "请使用蓝牙连接："       ~384 px
#   56 px  device name "8adge-XXXXX"          ~395 px
# The device name is always 11 characters, so 395 px is its worst case and the
# 498 px interior holds it with ~51 px to spare on each side. The sizes were
# picked for a longer 14-character name (452 px at 56 px), so the shorter brand
# only widened that margin - no size needs to change.
# Adding a size costs roughly (size/48)^2 * 12 KB of flash for the CJK pool.
SIZES = [28, 48, 56]

INK_THRESHOLD = 128


def is_latin(cp: int) -> bool:
    return 0x20 <= cp <= 0x7E


def build_charset() -> list[int]:
    chars: set[int] = set(ASCII_RANGE)
    for s in BLE_STRINGS:
        for ch in s:
            chars.add(ord(ch))
    return sorted(chars)


def rasterise_glyph(font, ch: str, ascent: int, descent: int):
    """Rasterise one character to a packed 1bpp bitmap.

    The glyph is drawn with its baseline at ``(pad, pad + ascent)`` on a scratch
    canvas and then cropped to its ink bounding box, so the returned ``x0`` /
    ``y0`` are the bitmap's top-left corner **relative to the baseline pen
    position** (``y0`` is normally negative, i.e. above the baseline).

    Returns ``(w, h, x0, y0, advance, rows)`` where ``rows`` holds
    ``ceil(w/8) * h`` bytes, MSB = leftmost pixel, bit set = ink.
    """
    advance = int(round(font.getlength(ch)))
    pad = max(8, (ascent + descent) // 4)
    canvas_w = max(advance, 1) + pad * 2
    canvas_h = ascent + descent + pad * 2

    canvas = Image.new("L", (canvas_w, canvas_h), 0)
    draw = ImageDraw.Draw(canvas)
    # anchor "ls" = left edge at the pen position, "s" = baseline
    draw.text((pad, pad + ascent), ch, font=font, fill=255, anchor="ls")

    bbox = canvas.getbbox()
    if bbox is None:
        return 0, 0, 0, 0, advance, []

    left, top, right, bottom = bbox
    w, h = right - left, bottom - top
    x0 = left - pad
    y0 = top - (pad + ascent)

    rows: list[int] = []
    px = canvas.load()
    row_bytes = (w + 7) // 8
    for row in range(h):
        for byte_i in range(row_bytes):
            byte = 0
            for bit in range(8):
                col = byte_i * 8 + bit
                if col >= w:
                    break
                if px[left + col, top + row] >= INK_THRESHOLD:
                    byte |= 0x80 >> bit
            rows.append(byte)
    return w, h, x0, y0, advance, rows


def emit(fonts_out: list[dict], out_path: str) -> None:
    lines: list[str] = []
    add = lines.append

    add("// Auto-generated by tools/gen_ble_font.py - DO NOT EDIT BY HAND.")
    add("//")
    add("// Subset bitmap font for the BLE hint screen. Contains only the")
    add("// characters in gen_ble_font.py's BLE_STRINGS plus printable ASCII,")
    add("// rasterised from assets/fonts/HarmonyOS_Sans_SC_Bold.ttf.")
    add("// Regenerate with:")
    add("//")
    add("//     python tools/gen_ble_font.py")
    add("")
    add("#ifndef BADGE8_BLE_FONT_H")
    add("#define BADGE8_BLE_FONT_H")
    add("")
    add("#include <stdint.h>")
    add("")
    add("// One glyph. Bitmaps are packed 1bpp rows, MSB = leftmost pixel, bit set")
    add("// = ink, and live at `bitmap + offset`.")
    add("typedef struct {")
    add("    uint16_t codepoint;   // Unicode scalar value")
    add("    uint8_t  w, h;        // bitmap size in pixels")
    add("    int8_t   x0, y0;      // bitmap top-left, relative to the baseline pen")
    add("    uint8_t  advance;     // pen advance in pixels")
    add("    uint16_t offset;      // byte offset into the font's bitmap pool")
    add("} BleGlyph;")
    add("")
    add("typedef struct {")
    add("    uint8_t  ascent;      // baseline distance from the top of the line box")
    add("    uint8_t  lineHeight;  // ascent + descent")
    add("    uint16_t glyphCount;")
    add("    const BleGlyph *glyphs;     // sorted by codepoint, binary-searchable")
    add("    const uint8_t  *bitmap;     // packed 1bpp glyph bitmaps")
    add("} BleFont;")
    add("")
    for f in fonts_out:
        add(f"extern const BleFont BLE_FONT_{f['size']};")
    add("")

    for f in fonts_out:
        size = f["size"]
        add("// " + "-" * 70)
        add(f"// {size} px")
        add("// " + "-" * 70)
        add(f"static const BleGlyph GLYPHS_{size}[] = {{")
        for g in f["glyphs"]:
            cp = g["cp"]
            label = chr(cp) if 0x20 <= cp <= 0x7E else f"U+{cp:04X}"
            label = label.replace("\\", "\\\\").replace('"', '\\"')
            add(
                f"    {{ 0x{cp:04X}, {g['w']:3d}, {g['h']:3d},"
                f" {g['x0']:4d}, {g['y0']:4d}, {g['advance']:3d},"
                f" {g['offset']:5d} }},  // {label}"
            )
        add("};")
        add("")

        pool = f["pool"]
        add(f"static const uint8_t BITMAP_{size}[] = {{")
        for i in range(0, len(pool), 16):
            chunk = pool[i : i + 16]
            add("    " + " ".join(f"0x{b:02X}," for b in chunk))
        add("};")
        add("")

        add(f"const BleFont BLE_FONT_{size} = {{")
        add(f"    {f['ascent']}, {f['line_height']}, {len(f['glyphs'])}, GLYPHS_{size}, BITMAP_{size}")
        add("};")
        add("")

    add("#endif // BADGE8_BLE_FONT_H")

    text = "\n".join(lines) + "\n"
    # utf-8 without BOM, LF endings: PlatformIO chokes on a BOM.
    with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print(f"wrote {out_path} ({len(text)} bytes)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ttf", default=DEFAULT_TTF)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--sizes", type=int, nargs="+", default=SIZES)
    args = ap.parse_args()

    if not os.path.isfile(args.ttf):
        return (f"missing font: {args.ttf}\n"
                f"(expected {os.path.join(FONT_DIR, 'HarmonyOS_Sans_SC_Bold.ttf')}; "
                f"pass --ttf to use another face)")

    charset = build_charset()
    cjk_cps = [cp for cp in charset if not is_latin(cp)]
    print(f"charset: {len(charset)} glyphs ({len(cjk_cps)} CJK) x {len(args.sizes)} sizes")
    print(f"  font  : {args.ttf}")

    fonts_out: list[dict] = []
    for size in args.sizes:
        font = ImageFont.truetype(args.ttf, size)
        ascent, descent = font.getmetrics()

        glyphs: list[dict] = []
        pool = bytearray()
        missing: list[int] = []
        for cp in charset:
            ch = chr(cp)
            w, h, x0, y0, advance, rows = rasterise_glyph(font, ch, ascent, descent)
            if w == 0 or h == 0:
                # The font has no ink for this codepoint (a blank glyph). That is
                # expected for the space character and a bug for anything else.
                if ch != " ":
                    missing.append(cp)
            glyphs.append(
                {
                    "cp": cp,
                    "w": w,
                    "h": h,
                    "x0": x0,
                    "y0": y0,
                    "advance": advance,
                    "offset": len(pool),
                }
            )
            pool.extend(rows)

            for name, value in (("w", w), ("h", h), ("advance", advance)):
                if value > 255:
                    return f"size {size}: {name}={value} for {ch!r} exceeds uint8_t"
            for name, value in (("x0", x0), ("y0", y0)):
                if not -128 <= value <= 127:
                    return f"size {size}: {name}={value} for {ch!r} exceeds int8_t"
        if len(pool) > 0xFFFF:
            return f"size {size}: bitmap pool {len(pool)} B exceeds uint16_t offsets"

        print(f"  {size:2d} px: {len(glyphs)} glyphs, pool {len(pool)} B, ascent {ascent}")
        if missing:
            print(
                "        !! blank glyphs for: "
                + " ".join(f"U+{cp:04X}({chr(cp)})" for cp in missing)
                + "  <- the font does not cover these; pick a font that does"
            )

        # Report how wide each screen string measures, so an over-wide line is
        # caught here instead of on the panel.
        by_cp = {g["cp"]: g for g in glyphs}
        for s in BLE_STRINGS:
            width = sum(by_cp[ord(ch)]["advance"] for ch in s)
            flag = "  <-- WIDER THAN 768" if width > 768 else ""
            print(f"        {width:4d} px  {s}{flag}")

        fonts_out.append(
            {
                "size": size,
                "glyphs": glyphs,
                "pool": pool,
                "ascent": ascent,
                "line_height": ascent + descent,
            }
        )

    emit(fonts_out, args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
