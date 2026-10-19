#!/usr/bin/env python3
"""Cross-check the protocol documentation against the firmware's own constants.

The doc quotes a lot of numbers that also live in C: UUIDs, payload lengths,
opcode values, the block geometry, the connection-interval units, and so on. Any
of them can drift when someone edits a header and forgets the prose - and the
document is what the Android app is written from, so a stale number there turns
into a bug on the phone that the firmware cannot explain.

So this script parses the headers, pulls out the constants that the doc is
supposed to quote, and asserts each one appears in the doc. It is deliberately
conservative: it only checks values that are *supposed* to be written down, and
it reports them in a table so a human can see what was actually verified.

    python tools/check_docs.py
"""

from __future__ import annotations

import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE_DIR = os.path.dirname(HERE)
DEFAULT_SRC = os.path.join(FIRMWARE_DIR, "src")
DEFAULT_DOC = os.path.join(FIRMWARE_DIR, "docs", "BLE_PROTOCOL.md")

FAILURES: list[str] = []


def defined_constants(src_dir: str) -> dict[str, str]:
    """`#define NAME value` pairs from the headers, value kept as written.

    String defines keep their surrounding quotes, which is never what the doc
    quotes - `#define BLE_CTRL_UUID "494e..."` has to be compared without them,
    or every UUID check reports a false gap.
    """
    out: dict[str, str] = {}
    for name in ("ble_protocol.h", "config.h", "epd_398_4color.h"):
        path = os.path.join(src_dir, name)
        text = open(path, encoding="utf-8").read()
        for m in re.finditer(r"^#define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(.+?)\s*(?://.*)?$",
                             text, re.M):
            value = m.group(2).strip()
            while len(value) >= 2 and value[0] == '"' and value[-1] == '"':
                value = value[1:-1]
            out.setdefault(m.group(1), value)
    return out


def enum_values(src_dir: str) -> dict[str, int]:
    """Scalar enum members from ble_protocol.h (BLE_OP_x = 0x01, BLE_STATE_x = 3...)."""
    text = open(os.path.join(src_dir, "ble_protocol.h"), encoding="utf-8").read()
    out: dict[str, int] = {}
    for m in re.finditer(r"^\s*(BLE_[A-Z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*,", text, re.M):
        out[m.group(1)] = int(m.group(2), 0)
    return out


def expect(doc: str, needle: str, note: str) -> None:
    if needle in doc:
        print(f"OK   {needle:<28} {note}")
    else:
        FAILURES.append(f"{needle}  ({note})")
        print(f"MISS {needle:<28} {note}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--src", default=DEFAULT_SRC)
    ap.add_argument("--doc", default=DEFAULT_DOC)
    args = ap.parse_args()

    doc = open(args.doc, encoding="utf-8").read()
    consts = defined_constants(args.src)
    enums = enum_values(args.src)

    print("=" * 78)
    print("Constants the doc must quote")
    print("=" * 78)

    # --- identity / version --------------------------------------------
    #
    # The two expect() calls below are what actually protect the document: they
    # compare it against the header, so a version bump cannot leave the prose
    # behind. There is deliberately no separate "must be 2" assertion any more -
    # that was just one more literal to remember on every protocol revision, and
    # it caught nothing the dynamic checks do not.
    proto = consts.get("BLE_PROTOCOL_VERSION", "?")
    expect(doc, f"版本：**协议 v{proto}**", "header blockquote: protocol version")
    expect(doc, f"协议版本          : {proto}", "constant cheat-sheet: protocol version")
    fw = (f'{consts.get("FW_VERSION_MAJOR", "?")}.'
          f'{consts.get("FW_VERSION_MINOR", "?")}.'
          f'{consts.get("FW_VERSION_PATCH", "?")}')
    expect(doc, f"固件版本          : {fw}", "constant cheat-sheet: firmware version")

    # --- UUIDs ----------------------------------------------------------
    for name in ("BLE_SERVICE_UUID", "BLE_CTRL_UUID", "BLE_DATA_UUID"):
        value = consts.get(name, "")
        if value:
            expect(doc, value, f"{name}")

    # --- canvas / payload lengths ---------------------------------------
    expect(doc, f'{consts.get("BLE_IMAGE_WIDTH", "?")} x {consts.get("BLE_IMAGE_HEIGHT", "?")}',
           "canvas size (BLE_IMAGE_WIDTH x BLE_IMAGE_HEIGHT)")
    for label, needle in (
        ("2bpp payload length", consts.get("BLE_FMT_2BPP_LEN_HINT", "")),
    ):
        if needle:
            expect(doc, needle, label)
    # The lengths are computed, so quote the numbers the headers imply.
    w = int(consts.get("BLE_IMAGE_WIDTH", "0"))
    h = int(consts.get("BLE_IMAGE_HEIGHT", "0"))
    if w and h:
        expect(doc, str(w * h // 4), f"2bpp payload = {w}x{h}/4")
        expect(doc, str(w * h // 8), f"1bpp payload = {w}x{h}/8")
        expect(doc, str(w // 4), "2bpp bytes per row")
        expect(doc, str(w // 8), "1bpp bytes per row")

    # --- block geometry -------------------------------------------------
    shift = int(consts.get("BLE_BLOCK_SHIFT_DEFAULT", "0"))
    lo = int(consts.get("BLE_BLOCK_SHIFT_MIN", "0"))
    hi = int(consts.get("BLE_BLOCK_SHIFT_MAX", "0"))
    block = 1 << shift if shift else 0
    expect(doc, str(lo), f"BLE_BLOCK_SHIFT_MIN ({lo} = {1 << lo} B)")
    expect(doc, str(hi), f"BLE_BLOCK_SHIFT_MAX ({hi} = {1 << hi} B)")
    expect(doc, str(consts.get("BLE_MAX_BLOCKS", "?")), "BLE_MAX_BLOCKS")
    expect(doc, f"`1 << 12` = {block} B" if False else f"{block} B",
           f"default block size = 1 << {shift}")
    # Block counts for the documented formats.
    if block:
        expect(doc, str((w * h // 4 + block - 1) // block), "2bpp block count")
        expect(doc, str((w * h // 8 + block - 1) // block), "1bpp block count")
        last = (w * h // 4) - ((w * h // 4) // block) * block
        if last:
            expect(doc, str(last), f"2bpp short final block ({last} B)")

    # --- chunking / window ----------------------------------------------
    expect(doc, str(consts.get("BLE_DATA_CHUNK_MAX", "?")), "BLE_DATA_CHUNK_MAX")
    expect(doc, str(consts.get("BLE_WINDOW_CHUNKS", "?")), "BLE_WINDOW_CHUNKS")
    expect(doc, str(consts.get("BLE_MTU_PREFERRED", "?")), "BLE_MTU_PREFERRED")

    # --- connection parameters ------------------------------------------
    ci_min = int(consts.get("BLE_CONN_MIN_INTERVAL", "0"))
    ci_max = int(consts.get("BLE_CONN_MAX_INTERVAL", "0"))
    expect(doc, f"{ci_min * 1.25:.2f}", f"min connection interval = {ci_min} * 1.25 ms")
    expect(doc, f"{ci_max * 1.25:.0f}", f"max connection interval = {ci_max} * 1.25 ms")
    expect(doc, str(int(consts.get("BLE_LL_TX_OCTETS", "0"))), "BLE_LL_TX_OCTETS")

    # --- refresh cooldown (v3) -------------------------------------------
    # The interval is a three-character number in a document full of numbers, so
    # matching the bare value would prove almost nothing. Check the field name
    # the app parses and the command names it has to implement instead, plus the
    # number the doc is supposed to quote.
    gap = consts.get("BLE_REFRESH_MIN_GAP_S", "?")
    expect(doc, "`refresh_min_gap_s`", "GET_INFO trailing field name")
    expect(doc, gap, f"BLE_REFRESH_MIN_GAP_S ({gap} s)")
    for name in ("BLE_OP_SYNC_TIME", "BLE_OP_CONFIRM_REFRESH"):
        expect(doc, name.replace("BLE_OP_", ""), f"{name} named in the doc")

    # --- opcodes ---------------------------------------------------------
    print()
    print("=" * 78)
    print("Opcodes (enum values vs. the doc's tables)")
    print("=" * 78)
    for name in ("BLE_OP_GET_INFO", "BLE_OP_BEGIN_IMAGE", "BLE_OP_END_IMAGE",
                 "BLE_OP_ABORT", "BLE_OP_SHOW_HINT", "BLE_OP_PING",
                 "BLE_OP_SET_OPTIONS", "BLE_OP_BLOCK_CRCS", "BLE_OP_RETRY_BLOCK",
                 "BLE_OP_SYNC_TIME", "BLE_OP_CONFIRM_REFRESH"):
        v = enums.get(name)
        if v is None:
            continue
        expect(doc, f"`0x{v:02X}`", f"{name}")
    for name in ("BLE_EVT_STATE", "BLE_EVT_PROGRESS", "BLE_EVT_LOG",
                 "BLE_EVT_REFRESH_BLOCKED"):
        v = enums.get(name)
        if v is None:
            continue
        expect(doc, f"`0x{v:02X}`", f"{name}")
    for name in ("BLE_ERR_BLOCKS", "BLE_ERR_CRC", "BLE_ERR_COOLDOWN", "BLE_ERR_MTU"):
        v = enums.get(name)
        if v is None:
            continue
        expect(doc, f"`0x{v:02X}`", f"{name}")
    for name in ("BLE_STATE_FIXING", "BLE_STATE_DISPLAYED"):
        v = enums.get(name)
        if v is None:
            continue
        # The state table writes the number bare ("| 9 | `FIXING` | ..."), so
        # match that shape rather than a backticked value.
        expect(doc, f"| {v} | `{name.replace('BLE_STATE_', '')}`", f"{name} (state row)")

    # --- things that must NOT still say v1 -------------------------------
    #
    # Only wording that would mislead an implementer if read as *current*
    # behaviour. The v1 row of the version table is allowed to describe v1's
    # stop-and-wait window, so "窗口流控" is not on this list.
    print()
    print("=" * 78)
    print("Stale v1 wording that must not survive")
    print("=" * 78)
    stale = [
        ("每发送这么多块，必须等一次", "old blocking-window wording"),
        ("`len = 45 + nameLen`", "old GET_INFO length"),
        ("`len = 29 + nameLen`", "older GET_INFO length"),
        ("`len = 37 + nameLen`", "v1/v2 GET_INFO length (v3 appends min gap)"),
        ("阻塞/挂起直到对应 ACK", "old wait-for-ACK sample code"),
        ("`EVT_WINDOW_ACK` | `u32 received, u32 total, u8 session`",
         "event table still naming the old event"),
    ]
    for needle, note in stale:
        if needle in doc:
            FAILURES.append(f"stale: {needle}")
            print(f"STALE {needle:<28} {note}")
        else:
            print(f"OK    (absent) {'':<19} {note}")

    # Old names are allowed as *historical* references, but only if the text
    # around them says so - a bare `EVT_WINDOW_ACK` would send the app author
    # down the wrong path.
    #
    # "Around them" means the enclosing section, not a character window: the
    # v1 -> v2 comparison table is nine rows long, so the marker that explains
    # it is in the heading, not within any sensible number of characters.
    print()
    headings = [(m.start(), m.group(0).strip())
                for m in re.finditer(r"^#{2,3} .*$", doc, re.M)]
    offenders = []
    for m in re.finditer(r"EVT_WINDOW_ACK", doc):
        section = ""
        for start, title in headings:
            if start < m.start():
                section = title
            else:
                break
        local = doc[max(0, m.start() - 120):m.end() + 120].replace("\n", " ")
        if any(mark in section for mark in ("v1", "旧")):
            continue
        if any(mark in local for mark in ("v1", "旧", "当时", "停止-等待")):
            continue
        offenders.append(f"section {section!r}: ...{local.strip()}...")
    if offenders:
        for ctx in offenders:
            FAILURES.append("EVT_WINDOW_ACK used without a v1 marker")
            print(f"STALE EVT_WINDOW_ACK     {ctx}")
    else:
        print("OK    (all EVT_WINDOW_ACK mentions sit in a v1-labelled context)")

    print()
    if FAILURES:
        print(f"{len(FAILURES)} PROBLEM(S):")
        for f in FAILURES:
            print(f"  - {f}")
        return 1
    print("DOC MATCHES THE FIRMWARE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
