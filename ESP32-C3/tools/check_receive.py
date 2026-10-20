#!/usr/bin/env python3
"""对 ble_service.cpp 中接收路径的字节级检查。

BLE 固件从不存储它收到的载荷：每个字节都经过 ble_geometry.h 中
竖屏->横屏的旋转，被直接散布到面板上的最终位置，同时一路累积
按块（4 KB 校验块）的 CRC，并在最后从帧缓冲重建整图
CRC。这其中任何一处出错都会产生一张微妙错误（subtly wrong）的图像 - 错位一行、
翻转一个角、某个块本不正确却被报成正确 - 而这些在 4 色面板上用肉眼都看不出来。

因此本脚本逐行转写相关的 C++ 代码，驱动它完成一次完整的传输（其中
包含一次刻意的损坏与修复），并把结果与一个参照实现进行比对，
后者做的是最直白的事（旋转整张画布，然后再打包）。检查项：

  1. 两个对外公布的载荷长度；
  2. 旋转是一个双射 - 每个面板像素恰好被写入一次；
  3. 散布路径逐字节复现“先旋转再打包”的结果（2bpp 和 1bpp）；
  4. 结果不依赖于分片边界落在哪里；
  5. 按块的 CRC 在正确的字节区间上累积；
  6. 一个被刻意损坏的块能被检测出来，且修复它后图像可以通过校验；
  7. 从帧缓冲重算的整图 CRC 等于对参照载荷计算的 CRC - 包括在修复之后，
     而这正是接收过程中累积的“运行中 CRC”会算错的情形；
  8. 短的最后一块在 RETRY_BLOCK 下应返回的字节数。

从任意位置运行：

    python tools/check_receive.py
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE_DIR = os.path.dirname(HERE)
DEFAULT_SRC = os.path.join(FIRMWARE_DIR, "src")

# ble_protocol.h / epd_398_4color.h 的镜像。
CANVAS_W, CANVAS_H = 552, 768
PANEL_W, PANEL_H = 768, 552
CANVAS_ROW_BYTES_2BPP = CANVAS_W // 4      # 138
CANVAS_ROW_BYTES_1BPP = CANVAS_W // 8      # 69
PANEL_ROW_BYTES = PANEL_W // 4             # 192
BYTES_2BPP = CANVAS_W * CANVAS_H // 4      # 105984
BYTES_1BPP = CANVAS_W * CANVAS_H // 8      # 52992
MAX_BLOCKS = 128
BLOCK_SHIFT_MIN, BLOCK_SHIFT_MAX = 11, 14

BLACK, WHITE, YELLOW, RED = 0, 1, 2, 3

FMT_2BPP, FMT_1BPP = 1, 2

FAILURES = 0


def check(ok: bool, label: str) -> None:
    global FAILURES
    if not ok:
        FAILURES += 1
    print(f"{'OK  ' if ok else 'FAIL'} {label}")


def read_rotation(src_dir: str) -> int:
    text = open(os.path.join(src_dir, "config.h"), encoding="utf-8").read()
    m = re.search(r"^#define\s+BLE_ROTATION\s+(\d+)", text, re.M)
    if not m:
        sys.exit("cannot find BLE_ROTATION in config.h")
    return int(m.group(1))


def read_window_chunks(src_dir: str) -> int:
    text = open(os.path.join(src_dir, "config.h"), encoding="utf-8").read()
    m = re.search(r"^#define\s+BLE_WINDOW_CHUNKS\s+(\d+)", text, re.M)
    return int(m.group(1)) if m else 32


def read_chunk_max(src_dir: str) -> int:
    text = open(os.path.join(src_dir, "config.h"), encoding="utf-8").read()
    m = re.search(r"^#define\s+BLE_DATA_CHUNK_MAX\s+(\d+)", text, re.M)
    return int(m.group(1)) if m else 512


def check_format_storage(src_dir: str) -> None:
    """会话格式绝不能存在 bool 里。

        BLE_FMT_2BPP 是 1、BLE_FMT_1BPP 是 2，所以 `static bool s_format` 会把
        格式 2 变成 `true`（== 1），让每一次 1bpp 传输都按 2bpp 解码并重新计算
        CRC。下面的字节级模型用哪种写法都会通过，因为它把格式当作参数传入 -
        正是本检查项钉住了 C++ 中的声明。
    """
    text = open(os.path.join(src_dir, "ble_service.cpp"), encoding="utf-8").read()
    m = re.search(r"^static\s+(?:volatile\s+)?(\w+)\s+s_format\s*=", text, re.M)
    check(m is not None, "ble_service.cpp declares s_format")
    if m:
        check(m.group(1) != "bool",
              f"s_format is {m.group(1)}, not bool (1bpp survived)")


def check_end_image_ok_length(src_dir: str) -> None:
    """END_IMAGE 的成功响应必须正好是文档所述的 8 字节。

        它过去传的是 sizeof(r)（26，即按失败位图开的数组大小），于是
        每一次成功的传输都会多发 18 字节未初始化的栈数据，而会校验长度的 App
        看到的会是一个畸形的帧，而不是一张完成的图像。
    """
    text = open(os.path.join(src_dir, "ble_service.cpp"), encoding="utf-8").read()
    m = re.search(r"sendResponse\(BLE_OP_END_IMAGE, seq, BLE_OK, r, ([^)]*)\)", text)
    check(m is not None, "ble_service.cpp has an END_IMAGE OK response")
    if m:
        arg = m.group(1).strip()
        check(arg != "sizeof(r)",
              "END_IMAGE OK payload length is not sizeof(r)")
        check(arg in ("OK_PAYLOAD_LEN", "8"),
              f"END_IMAGE OK payload length is {arg} (8 expected)")


def check_over_length_is_not_fatal(src_dir: str) -> None:
    """超过 total_len 的字节必须被忽略，而不是用来丢弃整张图像。"""
    text = open(os.path.join(src_dir, "ble_service.cpp"), encoding="utf-8").read()
    check("s_overflow" not in text,
          "no sticky s_overflow flag left in the receive path")
    check("s_strayBytes" in text,
          "over-length bytes are counted for the diagnosis")
    check("(s_received < s_total) ? (s_total - s_received) : 0" in text,
          "handleData() clamps the store to total_len")


def check_end_state_hint(src_dir: str) -> None:
    """没有会话时的 END_IMAGE 必须回 ERR_STATE + 那个 1 字节的提示。

        流水线式发送的 App 得不到别的信号：它多发的 DATA 被静默丢弃，
        后面也不会有 EVT_PROGRESS，所以没有这个字节，它就会永远等一个
        不会到来的 DISPLAYED。
    """
    text = open(os.path.join(src_dir, "ble_service.cpp"), encoding="utf-8").read()
    m = re.search(r"sendResponse\(BLE_OP_END_IMAGE, seq, BLE_ERR_STATE, ([^,]+), 1\)", text)
    check(m is not None and m.group(1).strip() in ("&hint", "&s_dataHint"),
          "END_IMAGE without a session answers ERR_STATE + data_hint")


class Panel:
    """固件的帧缓冲：面板原生方向，写入时经过旋转。"""

    def __init__(self, rotation: int) -> None:
        self.rotation = rotation
        self.buf = bytearray([0x55]) * (PANEL_W * PANEL_H // 4)
        self.writes = [0] * (PANEL_W * PANEL_H)

    # --- ble_geometry.h 的镜像 -----------------------------------------
    def map(self, x: int, y: int) -> tuple[int, int]:
        if self.rotation == 90:
            return (CANVAS_H - 1) - y, x
        return y, (CANVAS_W - 1) - x   # 270（180 在这里不是合法的旋转）

    def set_canvas(self, x: int, y: int, color: int) -> None:
        if not (0 <= x < CANVAS_W and 0 <= y < CANVAS_H):
            return
        px, py = self.map(x, y)
        idx = py * PANEL_ROW_BYTES + (px >> 2)
        shift = 6 - ((px & 3) * 2)
        self.buf[idx] = (self.buf[idx] & ~(0x03 << shift)) | ((color & 3) << shift)
        self.writes[py * PANEL_W + px] += 1

    def get_canvas(self, x: int, y: int) -> int:
        if not (0 <= x < CANVAS_W and 0 <= y < CANVAS_H):
            return WHITE
        px, py = self.map(x, y)
        idx = py * PANEL_ROW_BYTES + (px >> 2)
        return (self.buf[idx] >> (6 - ((px & 3) * 2))) & 0x03

    # --- ble_service.cpp::storePayloadByte 的镜像 -----------------------
    def store_payload_byte(self, offset: int, b: int, fmt: int) -> None:
        if fmt == FMT_2BPP:
            ys = offset // CANVAS_ROW_BYTES_2BPP
            xs = (offset % CANVAS_ROW_BYTES_2BPP) * 4
            self.set_canvas(xs + 0, ys, (b >> 6) & 3)
            self.set_canvas(xs + 1, ys, (b >> 4) & 3)
            self.set_canvas(xs + 2, ys, (b >> 2) & 3)
            self.set_canvas(xs + 3, ys, b & 3)
        else:
            ys = offset // CANVAS_ROW_BYTES_1BPP
            xs = (offset % CANVAS_ROW_BYTES_1BPP) * 8
            for k in range(8):
                white = ((b >> (7 - k)) & 1) != 0
                self.set_canvas(xs + k, ys, WHITE if white else BLACK)

    # --- ble_service.cpp::recomputeWholeCrc 的镜像 ----------------------
    #
        # 固件保存的是 *原始* CRC 状态（初值 0xFFFFFFFF，结束时异或）。
        # 而 zlib.crc32() 需要的是已经处理过的前一个值，所以忠实的镜像
        # 从 0 开始、且从不做异或：zlib.crc32(b, zlib.crc32(a))
        # == zlib.crc32(a + b)。若在这里从 0xFFFFFFFF 开始，就会对同样的字节
        # 静默地算出一个完全不同的数字。
    def recompute_whole_crc(self, fmt: int) -> int:
        crc = 0
        row = bytearray(CANVAS_ROW_BYTES_2BPP)
        if fmt == FMT_2BPP:
            for y in range(CANVAS_H):
                used = 0
                for x in range(0, CANVAS_W, 4):
                    row[used] = ((self.get_canvas(x + 0, y) << 6) |
                                 (self.get_canvas(x + 1, y) << 4) |
                                 (self.get_canvas(x + 2, y) << 2) |
                                 (self.get_canvas(x + 3, y)))
                    used += 1
                crc = zlib.crc32(bytes(row[:used]), crc)
        else:
            for y in range(CANVAS_H):
                used = 0
                for x in range(0, CANVAS_W, 8):
                    b = 0
                    for k in range(8):
                        if self.get_canvas(x + k, y) == WHITE:
                            b |= 0x80 >> k
                    row[used] = b
                    used += 1
                crc = zlib.crc32(bytes(row[:used]), crc)
        return crc


class Device:
    """ble_service.cpp 中传输状态机的转写。"""

    def __init__(self, palette: Panel, fmt: int, block_shift: int, total: int) -> None:
        self.p = palette
        self.fmt = fmt
        self.total = total
        self.block_shift = block_shift
        self.block_size = (1 << block_shift) if block_shift else 0
        self.block_count = ((total + self.block_size - 1) // self.block_size
                            if block_shift else 0)
        self.declared_blocks = [0] * MAX_BLOCKS
        self.begin_crc = None         # BEGIN_IMAGE 中声明的整图 CRC
        self.bad = bytearray((MAX_BLOCKS + 7) // 8)
        self.bad_count = 0
        self.block_index = 0
        self.block_crc = 0            # zlib 约定：0，而不是 0xFFFFFFFF
        self.block_bytes = 0
        self.received = 0
                # 超过 total_len 的字节：被忽略（它们无处可存），但会被
                # 计数，这样 END_IMAGE 就能在图像校验失败时指出它们。
                # 这里刻意不设粘滞的“溢出”状态 - 重发某个分片的 App
                # 不应该丢掉一张本来就已完整的图像。
        self.stray_bytes = 0
                # s_dataHint（ble_protocol.h 的 BLE_HINT_*）的镜像：下一次
                # 没有会话的 END_IMAGE 将告诉 App 的内容。由 BEGIN 清空。
        self.hint = 0
        self.fixing = False
        self.fix_block = 0
        self.fix_offset = 0
        self.fix_len = 0
        self.fix_needed = 0
        self.fix_crc = 0              # zlib 约定
        self.progress = []

    def _mark(self, index: int, bad_colour: bool) -> None:
        if index >= self.block_count:
            return
        mask = 1 << (index & 7)
        was = (self.bad[index >> 3] & mask) != 0
        if bad_colour and not was:
            self.bad[index >> 3] |= mask
            self.bad_count += 1
        elif not bad_colour and was:
            self.bad[index >> 3] &= ~mask & 0xFF
            if self.bad_count:
                self.bad_count -= 1

    def is_bad(self, index: int) -> bool:
        return index < self.block_count and (self.bad[index >> 3] & (1 << (index & 7))) != 0

    def begin_blocks(self, crcs: list[int], begin_crc: int | None = None) -> None:
        self.declared_blocks = list(crcs)
        self.begin_crc = begin_crc
        # resetTransfer()：新会话从干净状态开始。
        self.stray_bytes = 0
        self.hint = 0

    def data(self, chunk: bytes, window_chunks: int) -> None:
        n = len(chunk)
        if self.fixing:
            take = min(n, self.fix_needed)
            # 损坏偏移随着已经折入该块的字节前进，而不是随分片内的
                        # 局部下标。（用局部下标会把每个分片都钉在块起始处 - 正是
                        # 本工具在固件里抓到的那个 bug。）
            consumed = self.fix_len - self.fix_needed
            for i in range(take):
                self.p.store_payload_byte(self.fix_offset + consumed + i, chunk[i], self.fmt)
            self.fix_crc = zlib.crc32(chunk[:take], self.fix_crc)
            self.fix_needed -= take
            if self.fix_needed == 0:
                bad = self.fix_crc != self.declared_blocks[self.fix_block]
                self._mark(self.fix_block, bad)
                self.fixing = False
                self.progress.append((self.fix_offset, self.total, True))
            return

        # ble_service.cpp::handleData() 中钳位操作的镜像：只存能放下的，
                # 其余的被计数。改为整块拒绝（也就是“溢出 => 丢弃图像”的做法）
                # 是一种失败模式，本工具现在正是它的守卫。
        take = max(0, min(n, self.total - self.received))
        if take < n:
            self.stray_bytes += n - take
        if take == 0:
            # 即使什么都存不下，handleData() 也会执行到它的提示赋值语句，
                        # 因此一个完全超长的分片仍然会更新它。
            self.hint = 2 if self.stray_bytes else (1 if self.received >= self.total else 0)
            self.progress.append((self.received, self.total, False))
            return
        chunk = chunk[:take]
        n = take

        # 只遍历一遍 - 存储、折入当前块、在最后一个字节处收尾。
                # 见 ble_service.cpp 中的注释：把块的记账放到第二遍去做，
                # 会把边界之后的字节折进前一个块。
        for i in range(n):
            offset = self.received + i
            self.p.store_payload_byte(offset, chunk[i], self.fmt)
            if not self.block_shift:
                continue
            # 在新分片的第一个字节处复位，而不是在边界处复位，这样一次
                        # 完全丢失的写入会让 block_bytes == block_size，
                        # 从而让 close_missing_blocks() 能点出丢失的那个块。
            if self.block_bytes == self.block_size:
                self.block_crc = 0
                self.block_bytes = 0
            self.block_crc = zlib.crc32(chunk[i:i + 1], self.block_crc)
            self.block_bytes += 1
            if self.block_bytes == self.block_size or (offset + 1) == self.total:
                bad = self.block_crc != self.declared_blocks[self.block_index]
                self._mark(self.block_index, bad)
                self.block_index += 1
        self.received += n

        # handleData() 末尾那处提示赋值的镜像：一个没有会话时就到达的 END
                # 会报告的内容（文档 6.3）。
        self.hint = 2 if self.stray_bytes else (1 if self.received >= self.total else 0)

        self.progress.append((self.received, self.total, False))

    def close_missing_blocks(self) -> None:
        """closeMissingBlocks() 的镜像：点出那些字节从未到达的块。"""
        if not self.block_shift or not self.block_count:
            return
        if self.block_index >= self.block_count:
            return
        if self.block_bytes != 0:
            self._mark(self.block_index, True)
            self.block_index += 1
        while self.block_index < self.block_count:
            self._mark(self.block_index, True)
            self.block_index += 1
        self.block_crc = 0
        self.block_bytes = 0

    def retry_block(self, index: int) -> int:
        """handleRetryBlock() 的镜像；返回给 App 的字节数。

                0 = 该块本来就已经正确，-1 = ERR_BUSY（还有一次修复尚未完成：
                重新挂起会静默丢掉 App 已经发来的数据）。
        """
        if index >= self.block_count:
            raise AssertionError("index out of range")
        if self.fixing:
            return -1
        if not self.is_bad(index):
            return 0
        self.fix_block = index
        self.fix_offset = index * self.block_size
        remain = self.total - self.fix_offset
        self.fix_len = min(remain, self.block_size)
        self.fix_needed = self.fix_len
        self.fix_crc = 0
        self.fixing = True
        return self.fix_needed

    def end(self, declared_whole_crc: int) -> tuple[int, int]:
        """handleEndImage() 的镜像；返回 (status, payload_len)。"""
        self.close_missing_blocks()
        actual = self.p.recompute_whole_crc(self.fmt)
        complete = self.received == self.total
        blocks_enabled = self.block_shift != 0
        # BEGIN 和 END 必须描述同一个载荷。
        declared_ok = self.begin_crc is None or self.begin_crc == declared_whole_crc
        blocks_ok = (blocks_enabled and self.bad_count == 0
                     and self.block_index == self.block_count)
        ok = (blocks_ok and declared_ok and actual == declared_whole_crc
              if blocks_enabled
              else complete and declared_ok and actual == declared_whole_crc)
        if ok:
            return 0x00, 8
        if self.stray_bytes:
            # 是帧格式问题，不只是数据问题：到达的字节比声明的多，
                        # 而载荷也没能通过校验。8 字节的失败载荷，并且
                        # 会话仍然可以重传（它没有被复位）。
            return 0x0A, 8
        if blocks_enabled and self.bad_count > 0:
            return 0x0C, 10 + (self.block_count + 7) // 8
        return 0x06, 8


# ------------------------------------------------------------------ 测试数据

def make_canvas() -> list[list[int]]:
    """一张每个像素都携带自己坐标的画布。"""
    canvas = [[WHITE] * CANVAS_W for _ in range(CANVAS_H)]
    for y in range(CANVAS_H):
        for x in range(CANVAS_W):
            canvas[y][x] = ((x // 7) + (y // 11) * 2) & 0x03
    return canvas


def pack(canvas: list[list[int]], fmt: int) -> bytes:
    out = bytearray()
    if fmt == FMT_2BPP:
        for y in range(CANVAS_H):
            row = canvas[y]
            for x in range(0, CANVAS_W, 4):
                out.append((row[x] << 6) | (row[x + 1] << 4) | (row[x + 2] << 2) | row[x + 3])
    else:
        for y in range(CANVAS_H):
            row = canvas[y]
            for x in range(0, CANVAS_W, 8):
                b = 0
                for k in range(8):
                    if row[x + k] == WHITE:
                        b |= 0x80 >> k
                out.append(b)
    return bytes(out)


def threshold(canvas: list[list[int]]) -> list[list[int]]:
    return [[WHITE if c == WHITE else BLACK for c in row] for row in canvas]


def reference_panel(canvas: list[list[int]], rotation: int) -> bytes:
    """最直白的做法：旋转画好的画布，然后打包。"""
    panel = [[WHITE] * PANEL_W for _ in range(PANEL_H)]
    for y in range(CANVAS_H):
        for x in range(CANVAS_W):
            if rotation == 90:
                px, py = (CANVAS_H - 1) - y, x
            else:
                px, py = y, (CANVAS_W - 1) - x
            panel[py][px] = canvas[y][x]
    out = bytearray()
    for py in range(PANEL_H):
        row = panel[py]
        for px in range(0, PANEL_W, 4):
            out.append((row[px] << 6) | (row[px + 1] << 4) | (row[px + 2] << 2) | row[px + 3])
    return bytes(out)


def block_crcs(payload: bytes, block_size: int) -> list[tuple[int, int]]:
    """[(crc, byte_length), ...] - 最后一块可能较短。"""
    out = []
    for off in range(0, len(payload), block_size):
        chunk = payload[off:off + block_size]
        out.append((zlib.crc32(chunk) & 0xFFFFFFFF, len(chunk)))
    return out


def chunked(payload: bytes, size: int):
    for i in range(0, len(payload), size):
        yield payload[i:i + size]


def first_difference(a: bytes, b: bytes) -> str:
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            row = i // PANEL_ROW_BYTES
            col = (i % PANEL_ROW_BYTES) * 4
            return (f"byte {i} (panel y={row}, x={col}..{col + 3}): "
                    f"0x{a[i]:02X} vs 0x{b[i]:02X}")
    return "lengths differ"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--src", default=DEFAULT_SRC)
    ap.add_argument("--rotation", type=int, default=0,
                    help="override BLE_ROTATION (0 = read it from config.h); 90 or 270")
    ap.add_argument("--block-shift", type=int, default=12)
    args = ap.parse_args()

    rotation = args.rotation or read_rotation(args.src)
    if rotation not in (90, 270):
        sys.exit(f"BLE_ROTATION = {rotation} cannot map a portrait canvas onto this "
                 "panel; use 90 or 270")
    block_shift = args.block_shift
    if not BLOCK_SHIFT_MIN <= block_shift <= BLOCK_SHIFT_MAX:
        sys.exit(f"block shift {block_shift} outside the accepted "
                 f"{BLOCK_SHIFT_MIN}..{BLOCK_SHIFT_MAX}")
    block_size = 1 << block_shift
    window = read_window_chunks(args.src)
    chunk_max = read_chunk_max(args.src)

    print(f"canvas {CANVAS_W}x{CANVAS_H} -> panel {PANEL_W}x{PANEL_H}, rotation {rotation} deg CW")
    print(f"block size {block_size} B ({255 * 0 + block_shift}), window {window} chunks, "
          f"chunk max {chunk_max} B")
    print()

    # --- 0. 格式选择器本身 --------------------------------------------
    check_format_storage(args.src)
    # --- 0b. 本工具所守护的四个回归项 ----------------------------------
    check_end_image_ok_length(args.src)
    check_over_length_is_not_fatal(args.src)
    check_end_state_hint(args.src)

# --- 1. 载荷长度 ---------------------------------------------------
    check(BYTES_2BPP == PANEL_W * PANEL_H // 4,
          f"2bpp payload: {BYTES_2BPP} bytes = {CANVAS_ROW_BYTES_2BPP} x {CANVAS_H} rows")
    check(BYTES_1BPP == PANEL_W * PANEL_H // 8,
          f"1bpp payload: {BYTES_1BPP} bytes = {CANVAS_ROW_BYTES_1BPP} x {CANVAS_H} rows")

    # --- 2. 旋转是双射 -------------------------------------------------
    probe = Panel(rotation)
    seen = set()
    outside = 0
    for y in range(CANVAS_H):
        for x in range(CANVAS_W):
            px, py = probe.map(x, y)
            if not (0 <= px < PANEL_W and 0 <= py < PANEL_H):
                outside += 1
            else:
                seen.add((px, py))
    check(outside == 0 and len(seen) == PANEL_W * PANEL_H,
          f"rotation is a bijection ({len(seen)} distinct panel pixels, "
          f"{outside} outside); canvas (0,0) -> panel {probe.map(0, 0)}")

    canvas2 = make_canvas()
    canvas1 = threshold(canvas2)

    for fmt, payload, ref_canvas, label in (
        (FMT_2BPP, pack(canvas2, FMT_2BPP), canvas2, "2bpp"),
        (FMT_1BPP, pack(canvas1, FMT_1BPP), canvas1, "1bpp"),
    ):
        print()
        print(f"--- {label} ({len(payload)} bytes) ---")
        bcrcs = block_crcs(payload, block_size)
        blocks = len(bcrcs)

        panel = Panel(rotation)
        dev = Device(panel, fmt, block_shift, len(payload))

        # --- 3. 散布结果与“先旋转再打包”一致 ----------------------------
        dev.begin_blocks([c for c, _ in bcrcs])
        for ch in chunked(payload, chunk_max):
            dev.data(ch, window)
        ref = reference_panel(ref_canvas, rotation)
        same = bytes(panel.buf) == ref
        check(same, f"{label}: scatter path == rotate-then-pack"
                    + ("" if same else f"  <- {first_difference(bytes(panel.buf), ref)}"))

        missed = sum(1 for w in panel.writes if w != 1)
        check(missed == 0, f"{label}: every panel pixel written exactly once "
                           f"({missed} written 0 or >1 times)")

        # --- 5. 按块的 CRC 覆盖正确的区间 ------------------------------
        check(dev.bad_count == 0 and dev.block_count == blocks,
              f"{label}: all {blocks} block CRCs verified on a clean transfer")

        # --- 7. 重算整图 CRC --------------------------------------------
        got = panel.recompute_whole_crc(fmt)
        want = zlib.crc32(payload) & 0xFFFFFFFF
        check(got == want, f"{label}: recomputed whole CRC {got:08X} == payload CRC {want:08X}")

        status, plen = dev.end(want)
        check(status == 0x00 and plen == 8,
              f"{label}: END_IMAGE -> OK ({status:#04x}, {plen}-byte payload)")

        # --- 3b. BEGIN 和 END 必须对整图 CRC 达成一致 -------------------
        panel_x = Panel(rotation)
        dev_x = Device(panel_x, fmt, block_shift, len(payload))
        dev_x.begin_blocks([c for c, _ in bcrcs], begin_crc=(want ^ 0xFFFFFFFF))
        for ch in chunked(payload, chunk_max):
            dev_x.data(ch, window)
        status_x, _ = dev_x.end(want)
        check(status_x == 0x06,
              f"{label}: BEGIN/END whole-image CRC mismatch -> ERR_CRC")

        # --- 4. 分片边界无关紧要 ---------------------------------------
                # 是扫描而不是只测一次：分片大小不能整除块大小这一点
                # 正是暴露两遍式块记账 bug 的原因，而真实 MTU 落到一个
                # 别扭的尺寸时也必须照常工作。
        sweep_ok = True
        sweep_problems = []
        for size in (137, 251, 247, 512, 1000, 4096, 4097):
            panel_b = Panel(rotation)
            dev_b = Device(panel_b, fmt, block_shift, len(payload))
            dev_b.begin_blocks([c for c, _ in bcrcs])
            for ch in chunked(payload, size):
                dev_b.data(ch, window)
            if bytes(panel_b.buf) != ref or dev_b.bad_count != 0 or dev_b.received != len(payload):
                sweep_ok = False
                sweep_problems.append(f"{size}B:{dev_b.bad_count}bad")
        check(sweep_ok, f"{label}: identical pixels and all blocks verify across "
                        f"chunk sizes 137/251/247/512/1000/4096/4097"
                        + ("" if sweep_ok else f"  <- {' '.join(sweep_problems)}"))

        # --- 6. 损坏一个块、检测它、修复它 -----------------------------
        bad_index = 3
        off = bad_index * block_size
        corrupt = bytearray(payload)
        corrupt[off + 11] ^= 0xFF
        corrupt[off + 900] ^= 0x0F

        panel_c = Panel(rotation)
        dev_c = Device(panel_c, fmt, block_shift, len(payload))
        # App 声明的是它 *以为* 自己发出去的字节的 CRC，所以设备
                # 必须察觉实际到达的内容与之不同。
        dev_c.begin_blocks([c for c, _ in bcrcs])
        for ch in chunked(bytes(corrupt), chunk_max):
            dev_c.data(ch, window)
        status_c, plen_c = dev_c.end(want)
        check(dev_c.bad_count == 1 and dev_c.is_bad(bad_index),
              f"{label}: corrupted block {bad_index} detected "
              f"({dev_c.bad_count} bad of {dev_c.block_count})")
        check(status_c == 0x0C and plen_c == 10 + (dev_c.block_count + 7) // 8,
              f"{label}: END_IMAGE -> ERR_BLOCKS with a {plen_c}-byte bitmap payload")

        # 修复：只重发那一个块，从它自己的偏移开始。
        needed = dev_c.retry_block(bad_index)
        check(needed == bcrcs[bad_index][1],
              f"{label}: RETRY_BLOCK asks for {needed} bytes "
              f"(block {bad_index} is {bcrcs[bad_index][1]} long)")
        for ch in chunked(payload[off:off + needed], chunk_max):
            dev_c.data(ch, window)
        check(not dev_c.is_bad(bad_index),
              f"{label}: block {bad_index} verifies after the repair")
        check(bytes(panel_c.buf) == ref,
              f"{label}: repaired framebuffer is byte-identical to a clean transfer"
              + ("" if bytes(panel_c.buf) == ref
                 else f"  <- {first_difference(bytes(panel_c.buf), ref)}"))

        # --- 7b. 重算的 CRC 在修复之后依然成立 -------------------------
        got_after = panel_c.recompute_whole_crc(fmt)
        check(got_after == want,
              f"{label}: recomputed whole CRC after repair {got_after:08X} == {want:08X}")

        status_d, _ = dev_c.end(want)
        check(status_d == 0x00,
              f"{label}: END_IMAGE after repair -> OK (a running CRC accumulated "
              f"during reception could not have known this)")

        # --- 6b. 一次只修一个块 ---------------------------------------
                # 在第一个块还没收全时就挂起第二个块，会丢掉已经收到的
                # 那些字节，所以设备改为拒绝。
        panel_k = Panel(rotation)
        dev_k = Device(panel_k, fmt, block_shift, len(payload))
        dev_k.begin_blocks([c for c, _ in bcrcs])
        for ch in chunked(bytes(corrupt), chunk_max):
            dev_k.data(ch, window)
        dev_k.end(want)
        check(dev_k.retry_block(bad_index) > 0,
              f"{label}: the first repair is armed")
        half = max(1, bcrcs[bad_index][1] // 2)
        dev_k.data(payload[off:off + half], window)
        check(dev_k.retry_block(1) == -1,
              f"{label}: a second RETRY_BLOCK while one is outstanding -> ERR_BUSY")
        for ch in chunked(payload[off + half:off + bcrcs[bad_index][1]], chunk_max):
            dev_k.data(ch, window)
        check(dev_k.bad_count == 0,
              f"{label}: finishing the first repair still verifies the block")

        # --- 8. 短的最后一块 -------------------------------------------
        last = blocks - 1
        last_len = bcrcs[last][1]
        expected_last = len(payload) - last * block_size
        check(last_len == expected_last,
              f"{label}: final block {last} is {last_len} bytes "
              f"(payload {len(payload)} = {last} x {block_size} + {expected_last})")

        # --- 短的最后一块也必须能被检测到 ----------------------------
        corrupt_last = bytearray(payload)
        corrupt_last[len(payload) - 3] ^= 0x81
        panel_e = Panel(rotation)
        dev_e = Device(panel_e, fmt, block_shift, len(payload))
        dev_e.begin_blocks([c for c, _ in bcrcs])
        for ch in chunked(bytes(corrupt_last), chunk_max):
            dev_e.data(ch, window)
        dev_e.end(want)
        check(dev_e.is_bad(last) and dev_e.bad_count == 1,
              f"{label}: damage in the short final block is detected too")
        need_last = dev_e.retry_block(last)
        check(need_last == expected_last,
              f"{label}: RETRY_BLOCK for the final block asks for {need_last} bytes")
        for ch in chunked(payload[last * block_size:], chunk_max):
            dev_e.data(ch, window)
        check(dev_e.bad_count == 0 and bytes(panel_e.buf) == ref,
              f"{label}: final block repaired, framebuffer matches")

        # --- 9. 一次完全丢失的 DATA 写入 --------------------------------
                # 一次写入可能整条被丢掉（writeCharacteristic 返回 false、控制器
                # 缓冲溢出）。在这个缺口被补上之前，那些字节永远不到达，
                # 尾部块永远收不了尾，而 END 过去会一直只回一个
                # 干巴巴的 ERR_CRC。它必须改为点出缺失的块，并且
                # 对这些块做一次 RETRY_BLOCK 修复后必须能让图像通过校验，
                # 即使 s_received 永远到不了 total。
        chunks = list(chunked(payload, chunk_max))
        for dropped in (len(chunks) // 2, len(chunks) - 1):
            panel_g = Panel(rotation)
            dev_g = Device(panel_g, fmt, block_shift, len(payload))
            dev_g.begin_blocks([c for c, _ in bcrcs])
            for idx, ch in enumerate(chunks):
                if idx != dropped:
                    dev_g.data(ch, window)
            status_g, _ = dev_g.end(want)
            check(status_g == 0x0C and dev_g.bad_count > 0,
                  f"{label}: lost chunk {dropped} -> ERR_BLOCKS naming "
                  f"{dev_g.bad_count} block(s)")

            for i in range(dev_g.block_count):
                if not dev_g.is_bad(i):
                    continue
                need = dev_g.retry_block(i)
                start = i * block_size
                for ch in chunked(payload[start:start + need], chunk_max):
                    dev_g.data(ch, window)

            status_h, _ = dev_g.end(want)
            same_g = bytes(panel_g.buf) == ref
            check(status_h == 0x00 and same_g,
                  f"{label}: dropped chunk {dropped} repaired, END OK"
                  + ("" if same_g else f"  <- {first_difference(bytes(panel_g.buf), ref)}"))

        # --- 10. 超过 total_len 的字节被忽略，而不是致命错误 -----------
                # 重发最后一个分片的 App（因为它没看到进度上报）过去会因为
                # ERR_OVERFLOW 丢掉整张图像，哪怕每个像素和每个块的 CRC
                # 都正确。多出来的字节无处可存，所以它们被丢弃并计数。
        panel_i = Panel(rotation)
        dev_i = Device(panel_i, fmt, block_shift, len(payload))
        dev_i.begin_blocks([c for c, _ in bcrcs])
        for ch in chunks:
            dev_i.data(ch, window)
        dev_i.data(chunks[-1], window)          # 这次重复
        status_i, plen_i = dev_i.end(want)
        check(status_i == 0x00 and plen_i == 8 and dev_i.stray_bytes == len(chunks[-1]),
              f"{label}: a repeated final chunk is ignored "
              f"({dev_i.stray_bytes} stray bytes) and the image still verifies")
        check(bytes(panel_i.buf) == ref,
              f"{label}: the repeat left the framebuffer untouched")

        # --- 10b. 超长 + 一个真实缺陷 -> ERR_OVERFLOW ------------------
                # 这种宽容绝不能削弱诊断能力：如果载荷同时也校验失败，
                # 那么告诉 App 的就是超长的帧格式问题（而且会话保持可重传，
                # 不会被复位）。
        panel_j = Panel(rotation)
        dev_j = Device(panel_j, fmt, block_shift, len(payload))
        dev_j.begin_blocks([c for c, _ in bcrcs])
        dev_j.data(bytes(corrupt), window)      # 来自测试 6 的损坏块
        for ch in chunks[1:]:
            dev_j.data(ch, window)
        dev_j.data(chunks[-1], window)
        status_j, plen_j = dev_j.end(want)
        check(status_j == 0x0A and plen_j == 8 and dev_j.stray_bytes > 0,
              f"{label}: over-length plus a bad block -> ERR_OVERFLOW "
              f"({status_j:#04x}, {plen_j} bytes)")

        # --- 10c. data_hint 契约（文档 6.3）----------------------------
                # 流水线式发送的 App 没有别的信号，所以这个字节就是
                # “等 DISPLAYED” 和 “重新开始” 之间的分界。
        check(dev_i.hint == 2,
              f"{label}: over-length bytes leave the hint at DATA_IGNORED "
              f"({dev_i.hint})")
        check(dev.hint == 1,
              f"{label}: a completed payload leaves the hint at DATA_AFTER_END "
              f"({dev.hint})")
        dev_i.begin_blocks([c for c, _ in bcrcs])
        check(dev_i.hint == 0 and dev_i.stray_bytes == 0,
              f"{label}: BEGIN_IMAGE clears the hint and the stray-byte count")

    print()
    print("--- block shift sweep ---")
    for shift in range(BLOCK_SHIFT_MIN, BLOCK_SHIFT_MAX + 1):
        size = 1 << shift
        n = (BYTES_2BPP + size - 1) // size
        ok = n <= MAX_BLOCKS
        check(ok, f"shift {shift} ({size} B) -> {n} blocks (limit {MAX_BLOCKS})")

    print()
    if FAILURES:
        print(f"{FAILURES} CHECK(S) FAILED")
        return 1
    print("ALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
