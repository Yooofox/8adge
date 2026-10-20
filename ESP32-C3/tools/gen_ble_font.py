#!/usr/bin/env python3
"""生成 BLE 提示屏所用的子集点阵字体。

BLE 固件没有 WiFi，因此也没有强制门户，但它仍然必须把一句中文
放到面板上：

    请使用蓝牙连接：
    8adge-XXXXX
    打开手机App搜索并连接

放在它 552x768 的竖屏画布上 - 见 `ble_screen.cpp`。固件内置的
文字路径（`drawText()`）是手写的 5x7 ASCII 字体，所以中文需要一份
栅格化的子集。本脚本不嵌入整套 CJK 字面，而是只栅格化
**那三行用到的字符** 加上完整的可打印 ASCII（设备名是在运行时
由 MAC 地址构造的，所以每个十六进制数字都必须能渲染），并输出一张
扁平的、可二分查找的字形表。

源字面随本项目一起提供，因此重建字体不需要项目文件夹
之外的任何东西：

    assets/fonts/HarmonyOS_Sans_SC_Bold.ttf   (HarmonyOS Sans Fonts License)

请使用 SC 子族，而不是基础的 ``HarmonyOS_Sans`` 字面：后者只有 874
个码位且 **完全没有中文**，那会让每个标签都渲染成空白。

从任意位置运行（需要 Pillow - ``pip install pillow``）：

    python tools/gen_ble_font.py

它会重写 ``src/ble_font.h``。每当 BLE_STRINGS 或 SIZES 变化时都要重新运行。
"""

from __future__ import annotations

import argparse
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:  # pragma: no cover - 依赖提示
    sys.exit("Pillow is required: pip install pillow")

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE_DIR = os.path.dirname(HERE)
FONT_DIR = os.path.join(FIRMWARE_DIR, "assets", "fonts")

DEFAULT_TTF = os.path.join(FONT_DIR, "HarmonyOS_Sans_SC_Bold.ttf")
DEFAULT_OUT = os.path.join(FIRMWARE_DIR, "src", "ble_font.h")

# ---------------------------------------------------------------- 字符集
# 提示屏绘制的全部内容。与 ble_screen.cpp 保持同步。
# 全角冒号 U+FF1A 是刻意使用的：屏幕文字要的就是它，
# 它与 ASCII 的 ':' 不是同一个字形。
BLE_STRINGS = [
    "请使用蓝牙连接：",
    "8adge-XXXXX",
    "打开手机App搜索并连接",
]

# 可打印 ASCII，这样运行时构造出的设备名（"8adge-A1B2C"）总能
# 渲染出来。
ASCII_RANGE = range(0x20, 0x7F)

# ---------------------------------------------------------------- 字号
# 像素字号，是针对 ble_screen.cpp 中排布的 552x768 竖屏画布选定的
# （圆环 24 px + 3 px 细边线，剩下 498 px 的可用宽度）。
# 这些字号下实测的宽度（见运行时的报告）：
#   28 px  页脚      "打开手机App搜索并连接"  ~308 px
#   48 px  标题      "请使用蓝牙连接："       ~384 px
#   56 px  设备名    "8adge-XXXXX"          ~395 px
# 设备名始终是 11 个字符，所以 395 px 是它的最坏情况，498 px 的
# 内部宽度容纳它后每边还富余约 51 px。这些字号当初是按更长的
# 14 字符名称（56 px 下 452 px）挑选的，所以更短的品牌名只会让这个
# 余量更大 - 没有任何字号需要改动。
# 每增加一个字号，CJK 字形池大约多消耗 (size/48)^2 * 12 KB 的 flash。
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
    """把一个字符栅格化成打包的 1bpp 位图。

        字形以基线落在 ``(pad, pad + ascent)`` 的方式画在一张草稿
        画布上，然后裁剪到它的墨迹外接框，因此返回的 ``x0`` /
        ``y0`` 是该位图左上角 **相对于基线笔位置** 的坐标
        （``y0`` 通常为负，即在基线之上）。

        返回 ``(w, h, x0, y0, advance, rows)``，其中 ``rows`` 含
        ``ceil(w/8) * h`` 字节，MSB = 最左侧像素，位为 1 = 墨迹。
    """
    advance = int(round(font.getlength(ch)))
    pad = max(8, (ascent + descent) // 4)
    canvas_w = max(advance, 1) + pad * 2
    canvas_h = ascent + descent + pad * 2

    canvas = Image.new("L", (canvas_w, canvas_h), 0)
    draw = ImageDraw.Draw(canvas)
    # anchor "ls" = 左边缘在笔位置，"s" = 基线
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

    add("// 由 tools/gen_ble_font.py 自动生成——请勿手改。")
    add("//")
    add("// BLE 提示屏用的子集点阵字库。只包含 gen_ble_font.py 的")
    add("// BLE_STRINGS 里的字符加上可打印 ASCII，")
    add("// 由 assets/fonts/HarmonyOS_Sans_SC_Bold.ttf 栅格化而来。")
    add("// 重新生成：")
    add("//")
    add("//     python tools/gen_ble_font.py")
    add("")
    add("#ifndef BADGE8_BLE_FONT_H")
    add("#define BADGE8_BLE_FONT_H")
    add("")
    add("#include <stdint.h>")
    add("")
    add("// 一个字形。点阵按 1bpp 逐行打包，MSB = 最左像素，置位 = 上墨，")
    add("// 数据位于 `bitmap + offset`。")
    add("typedef struct {")
    add("    uint16_t codepoint;   // Unicode 码点")
    add("    uint8_t  w, h;        // 点阵尺寸（像素）")
    add("    int8_t   x0, y0;      // 点阵左上角，相对基线的落笔点")
    add("    uint8_t  advance;     // 落笔前进量（像素）")
    add("    uint16_t offset;      // 在字库点阵池里的字节偏移")
    add("} BleGlyph;")
    add("")
    add("typedef struct {")
    add("    uint8_t  ascent;      // 基线到行框顶部的距离")
    add("    uint8_t  lineHeight;  // ascent + descent")
    add("    uint16_t glyphCount;")
    add("    const BleGlyph *glyphs;     // 按码点排序，可二分查找")
    add("    const uint8_t  *bitmap;     // 打包好的 1bpp 字形点阵")
    add("} BleFont;")
    add("")
    for f in fonts_out:
        add(f"extern const BleFont BLE_FONT_{f['size']};")
    add("")

    for f in fonts_out:
        size = f["size"]
        add("// " + "-" * 70)
        add(f"// {size} 像素")
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
    # utf-8 无 BOM，LF 换行：PlatformIO 遇到 BOM 会报错。
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
                # 该字体对这个码位没有墨迹（一个空白字形）。这对
                                # 空格键字符来说是预期的，对其他任何字符则是 bug。
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

        # 报告每个屏幕字符串测得有多宽，这样过宽的行就会
                # 在这里而不是在面板上被抓到。
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
