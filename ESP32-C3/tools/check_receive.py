#!/usr/bin/env python3
"""Byte-level check of the receive path in ble_service.cpp.

The BLE firmware never stores the payload it receives: every byte is scattered
straight into its final panel position, through the portrait->landscape rotation
in ble_geometry.h, with per-block CRCs accumulated as it goes and the whole-image
CRC reconstructed from the framebuffer at the end. A mistake in any of that
produces an image that is subtly wrong - a shifted row, a flipped corner, a block
reported good when it is not - and none of it is visible by eye on a 4-colour
panel.

So this script transcribes the relevant C++ line by line, drives it through a
full transfer including a deliberate corruption and repair, and checks the result
against a reference implementation that does the obvious thing (rotate the whole
canvas, then pack). Checks:

  1. the two advertised payload lengths;
  2. the rotation is a bijection - every panel pixel written exactly once;
  3. the scatter path reproduces rotate-then-pack byte for byte (2bpp and 1bpp);
  4. the result does not depend on where chunk boundaries fall;
  5. per-block CRCs are accumulated over the right byte ranges;
  6. a deliberately corrupted block is detected, and repairing it makes the
     image verify;
  7. the whole-image CRC recomputed from the framebuffer equals a CRC over the
     reference payload - including after a repair, which is the case a running
     CRC accumulated during reception would get wrong;
  8. RETRY_BLOCK's byte count for the short final block.

Run from anywhere:

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

# Mirror of ble_protocol.h / epd_398_4color.h.
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
    """The session format must not be stored in a bool.

    BLE_FMT_2BPP is 1 and BLE_FMT_1BPP is 2, so `static bool s_format` turns
    format 2 into `true` (== 1) and makes every 1bpp transfer decode and re-CRC
    as 2bpp. The byte-level model below passes either way because it takes the
    format as an argument - this check is what pins the C++ declaration.
    """
    text = open(os.path.join(src_dir, "ble_service.cpp"), encoding="utf-8").read()
    m = re.search(r"^static\s+(?:volatile\s+)?(\w+)\s+s_format\s*=", text, re.M)
    check(m is not None, "ble_service.cpp declares s_format")
    if m:
        check(m.group(1) != "bool",
              f"s_format is {m.group(1)}, not bool (1bpp survived)")


def check_end_image_ok_length(src_dir: str) -> None:
    """The END_IMAGE success reply must be exactly the documented 8 bytes.

    It used to pass sizeof(r) (26, the array sized for the failure bitmap), so
    every successful transfer shipped 18 bytes of uninitialised stack and an app
    that validated the length saw a malformed frame instead of a finished image.
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
    """Bytes past total_len must be ignored, not used to discard the image."""
    text = open(os.path.join(src_dir, "ble_service.cpp"), encoding="utf-8").read()
    check("s_overflow" not in text,
          "no sticky s_overflow flag left in the receive path")
    check("s_strayBytes" in text,
          "over-length bytes are counted for the diagnosis")
    check("(s_received < s_total) ? (s_total - s_received) : 0" in text,
          "handleData() clamps the store to total_len")


def check_end_state_hint(src_dir: str) -> None:
    """END_IMAGE with no session must answer ERR_STATE + the 1-byte hint.

    A pipelining app gets no other signal: its extra DATA is dropped in silence
    and no EVT_PROGRESS follows, so without this byte it waits forever for a
    DISPLAYED that will never come.
    """
    text = open(os.path.join(src_dir, "ble_service.cpp"), encoding="utf-8").read()
    m = re.search(r"sendResponse\(BLE_OP_END_IMAGE, seq, BLE_ERR_STATE, ([^,]+), 1\)", text)
    check(m is not None and m.group(1).strip() in ("&hint", "&s_dataHint"),
          "END_IMAGE without a session answers ERR_STATE + data_hint")


class Panel:
    """The firmware framebuffer: panel-native, written through the rotation."""

    def __init__(self, rotation: int) -> None:
        self.rotation = rotation
        self.buf = bytearray([0x55]) * (PANEL_W * PANEL_H // 4)
        self.writes = [0] * (PANEL_W * PANEL_H)

    # --- mirror of ble_geometry.h -------------------------------------
    def map(self, x: int, y: int) -> tuple[int, int]:
        if self.rotation == 90:
            return (CANVAS_H - 1) - y, x
        return y, (CANVAS_W - 1) - x   # 270 (180 is not a valid rotation here)

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

    # --- mirror of ble_service.cpp::storePayloadByte -------------------
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

    # --- mirror of ble_service.cpp::recomputeWholeCrc ------------------
    #
    # The firmware keeps the *raw* CRC state (init 0xFFFFFFFF, xor at the end).
    # zlib.crc32() wants the already-conditioned previous value instead, so the
    # faithful mirror starts from 0 and never xors: zlib.crc32(b, zlib.crc32(a))
    # == zlib.crc32(a + b). Starting from 0xFFFFFFFF here would silently produce
    # a completely different number for the same bytes.
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
    """Transcription of the transfer state machine in ble_service.cpp."""

    def __init__(self, palette: Panel, fmt: int, block_shift: int, total: int) -> None:
        self.p = palette
        self.fmt = fmt
        self.total = total
        self.block_shift = block_shift
        self.block_size = (1 << block_shift) if block_shift else 0
        self.block_count = ((total + self.block_size - 1) // self.block_size
                            if block_shift else 0)
        self.declared_blocks = [0] * MAX_BLOCKS
        self.begin_crc = None         # whole-image CRC declared in BEGIN_IMAGE
        self.bad = bytearray((MAX_BLOCKS + 7) // 8)
        self.bad_count = 0
        self.block_index = 0
        self.block_crc = 0            # zlib convention: 0, not 0xFFFFFFFF
        self.block_bytes = 0
        self.received = 0
        # Bytes past total_len: ignored (they cannot be stored anywhere) but
        # counted, so END_IMAGE can name them when the image fails to verify.
        # There is deliberately no sticky "overflow" state - an app that repeats
        # a chunk must not lose an image that was already complete.
        self.stray_bytes = 0
        # Mirror of s_dataHint (ble_protocol.h BLE_HINT_*): what the next
        # END_IMAGE with no session will tell the app. Cleared by BEGIN.
        self.hint = 0
        self.fixing = False
        self.fix_block = 0
        self.fix_offset = 0
        self.fix_len = 0
        self.fix_needed = 0
        self.fix_crc = 0              # zlib convention
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
        # resetTransfer(): a new session starts with a clean slate.
        self.stray_bytes = 0
        self.hint = 0

    def data(self, chunk: bytes, window_chunks: int) -> None:
        n = len(chunk)
        if self.fixing:
            take = min(n, self.fix_needed)
            # Damage offset advances with the bytes already folded into the
            # block, not with the chunk-local index. (Using the local index
            # pinned every chunk to the block start - the bug this tool caught
            # in the firmware.)
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

        # Mirror of the clamp in ble_service.cpp::handleData(): only what fits is
        # stored, the rest is counted. Rejecting the whole chunk instead (which
        # is what "overflow => discard the image" amounted to) is a failure mode
        # this tool now guards against.
        take = max(0, min(n, self.total - self.received))
        if take < n:
            self.stray_bytes += n - take
        if take == 0:
            # handleData() reaches its hint assignment even when nothing could be
            # stored, so a wholly over-length chunk still updates it.
            self.hint = 2 if self.stray_bytes else (1 if self.received >= self.total else 0)
            self.progress.append((self.received, self.total, False))
            return
        chunk = chunk[:take]
        n = take

        # ONE pass - store, roll into the current block, close on the last byte.
        # See the comment in ble_service.cpp: doing the block bookkeeping in a
        # second pass folds the bytes after a boundary into the previous block.
        for i in range(n):
            offset = self.received + i
            self.p.store_payload_byte(offset, chunk[i], self.fmt)
            if not self.block_shift:
                continue
            # Reset on the first byte of a new chunk, not at the boundary, so a
            # completely lost write leaves block_bytes == block_size for
            # close_missing_blocks() to name the lost block.
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

        # Mirror of the hint assignment at the end of handleData(): what an END
        # arriving with no session would report (docs 6.3).
        self.hint = 2 if self.stray_bytes else (1 if self.received >= self.total else 0)

        self.progress.append((self.received, self.total, False))

    def close_missing_blocks(self) -> None:
        """Mirror of closeMissingBlocks(): name blocks whose bytes never came."""
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
        """Mirror of handleRetryBlock(); returns the byte count for the app."""
        if index >= self.block_count:
            raise AssertionError("index out of range")
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
        """Mirror of handleEndImage(); returns (status, payload_len)."""
        self.close_missing_blocks()
        actual = self.p.recompute_whole_crc(self.fmt)
        complete = self.received == self.total
        blocks_enabled = self.block_shift != 0
        # BEGIN and END must describe the same payload.
        declared_ok = self.begin_crc is None or self.begin_crc == declared_whole_crc
        blocks_ok = (blocks_enabled and self.bad_count == 0
                     and self.block_index == self.block_count)
        ok = (blocks_ok and declared_ok and actual == declared_whole_crc
              if blocks_enabled
              else complete and declared_ok and actual == declared_whole_crc)
        if ok:
            return 0x00, 8
        if self.stray_bytes:
            # Framing, not just data: more bytes arrived than were declared and
            # the payload did not verify either. 8-byte failure payload, and the
            # session stays retryable (it is not reset).
            return 0x0A, 8
        if blocks_enabled and self.bad_count > 0:
            return 0x0C, 10 + (self.block_count + 7) // 8
        return 0x06, 8


# ------------------------------------------------------------------ test data

def make_canvas() -> list[list[int]]:
    """A canvas whose every pixel carries its own coordinates."""
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
    """The obvious way: rotate the finished canvas, then pack."""
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
    """[(crc, byte_length), ...] - the last block may be short."""
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

    # --- 0. the format selector itself --------------------------------
    check_format_storage(args.src)
    # --- 0b. the four regressions this tool is the guard for ----------
    check_end_image_ok_length(args.src)
    check_over_length_is_not_fatal(args.src)
    check_end_state_hint(args.src)

# --- 1. payload lengths -------------------------------------------
    check(BYTES_2BPP == PANEL_W * PANEL_H // 4,
          f"2bpp payload: {BYTES_2BPP} bytes = {CANVAS_ROW_BYTES_2BPP} x {CANVAS_H} rows")
    check(BYTES_1BPP == PANEL_W * PANEL_H // 8,
          f"1bpp payload: {BYTES_1BPP} bytes = {CANVAS_ROW_BYTES_1BPP} x {CANVAS_H} rows")

    # --- 2. rotation is a bijection -----------------------------------
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

        # --- 3. scatter matches rotate-then-pack ----------------------
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

        # --- 5. block CRCs over the right ranges ----------------------
        check(dev.bad_count == 0 and dev.block_count == blocks,
              f"{label}: all {blocks} block CRCs verified on a clean transfer")

        # --- 7. recomputed whole CRC --------------------------------
        got = panel.recompute_whole_crc(fmt)
        want = zlib.crc32(payload) & 0xFFFFFFFF
        check(got == want, f"{label}: recomputed whole CRC {got:08X} == payload CRC {want:08X}")

        status, plen = dev.end(want)
        check(status == 0x00 and plen == 8,
              f"{label}: END_IMAGE -> OK ({status:#04x}, {plen}-byte payload)")

        # --- 3b. BEGIN and END must agree on the whole-image CRC -------
        panel_x = Panel(rotation)
        dev_x = Device(panel_x, fmt, block_shift, len(payload))
        dev_x.begin_blocks([c for c, _ in bcrcs], begin_crc=(want ^ 0xFFFFFFFF))
        for ch in chunked(payload, chunk_max):
            dev_x.data(ch, window)
        status_x, _ = dev_x.end(want)
        check(status_x == 0x06,
              f"{label}: BEGIN/END whole-image CRC mismatch -> ERR_CRC")

        # --- 4. chunk boundaries do not matter -----------------------
        # Swept, not just tested once: a chunk size that does not divide the
        # block size is what exposed the two-pass block bookkeeping bug, and a
        # real MTU that lands on an awkward size must keep working.
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

        # --- 6. corrupt a block, detect it, repair it -----------------
        bad_index = 3
        off = bad_index * block_size
        corrupt = bytearray(payload)
        corrupt[off + 11] ^= 0xFF
        corrupt[off + 900] ^= 0x0F

        panel_c = Panel(rotation)
        dev_c = Device(panel_c, fmt, block_shift, len(payload))
        # The app declares the CRCs of what it *thinks* it sent, so the device
        # must notice that what arrived is different.
        dev_c.begin_blocks([c for c, _ in bcrcs])
        for ch in chunked(bytes(corrupt), chunk_max):
            dev_c.data(ch, window)
        status_c, plen_c = dev_c.end(want)
        check(dev_c.bad_count == 1 and dev_c.is_bad(bad_index),
              f"{label}: corrupted block {bad_index} detected "
              f"({dev_c.bad_count} bad of {dev_c.block_count})")
        check(status_c == 0x0C and plen_c == 10 + (dev_c.block_count + 7) // 8,
              f"{label}: END_IMAGE -> ERR_BLOCKS with a {plen_c}-byte bitmap payload")

        # Repair: resend only that block, from its own offset.
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

        # --- 7b. the recomputed CRC survives the repair ---------------
        got_after = panel_c.recompute_whole_crc(fmt)
        check(got_after == want,
              f"{label}: recomputed whole CRC after repair {got_after:08X} == {want:08X}")

        status_d, _ = dev_c.end(want)
        check(status_d == 0x00,
              f"{label}: END_IMAGE after repair -> OK (a running CRC accumulated "
              f"during reception could not have known this)")

        # --- 8. the short final block ---------------------------------
        last = blocks - 1
        last_len = bcrcs[last][1]
        expected_last = len(payload) - last * block_size
        check(last_len == expected_last,
              f"{label}: final block {last} is {last_len} bytes "
              f"(payload {len(payload)} = {last} x {block_size} + {expected_last})")

        # --- a short last block must still be detectable --------------
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

        # --- 9. a completely lost DATA write ---------------------------
        # A write can be dropped whole (writeCharacteristic returned false, the
        # controller buffer overflowed). Until the gap is closed those bytes
        # never arrive, the tail block never closes, and END used to answer a
        # bare ERR_CRC forever. It must name the missing blocks instead, and a
        # RETRY_BLOCK repair of those blocks must make the image verify even
        # though s_received never reaches total.
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

        # --- 10. bytes past total_len are ignored, not fatal -----------
        # An app that re-sends its last chunk (because it never saw the progress
        # report) used to lose the whole image to ERR_OVERFLOW even though every
        # pixel and every block CRC were correct. The extra bytes cannot be
        # stored anywhere, so they are dropped and counted.
        panel_i = Panel(rotation)
        dev_i = Device(panel_i, fmt, block_shift, len(payload))
        dev_i.begin_blocks([c for c, _ in bcrcs])
        for ch in chunks:
            dev_i.data(ch, window)
        dev_i.data(chunks[-1], window)          # the repeat
        status_i, plen_i = dev_i.end(want)
        check(status_i == 0x00 and plen_i == 8 and dev_i.stray_bytes == len(chunks[-1]),
              f"{label}: a repeated final chunk is ignored "
              f"({dev_i.stray_bytes} stray bytes) and the image still verifies")
        check(bytes(panel_i.buf) == ref,
              f"{label}: the repeat left the framebuffer untouched")

        # --- 10b. over-length + a real defect -> ERR_OVERFLOW ----------
        # The tolerance must not weaken the diagnosis: if the payload also fails
        # to verify, the over-length framing is what the app is told about (and
        # it stays retryable instead of being reset).
        panel_j = Panel(rotation)
        dev_j = Device(panel_j, fmt, block_shift, len(payload))
        dev_j.begin_blocks([c for c, _ in bcrcs])
        dev_j.data(bytes(corrupt), window)      # corrupt block from test 6
        for ch in chunks[1:]:
            dev_j.data(ch, window)
        dev_j.data(chunks[-1], window)
        status_j, plen_j = dev_j.end(want)
        check(status_j == 0x0A and plen_j == 8 and dev_j.stray_bytes > 0,
              f"{label}: over-length plus a bad block -> ERR_OVERFLOW "
              f"({status_j:#04x}, {plen_j} bytes)")

        # --- 10c. the data_hint contract (docs 6.3) --------------------
        # A pipelining app has no other signal, so this byte is the difference
        # between "wait for DISPLAYED" and "start over".
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
