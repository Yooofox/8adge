#!/usr/bin/env python3
"""把协议文档与固件自身的常量做交叉核对。

文档里引用了大量同样存在于 C 代码中的数字：UUID、载荷长度、
opcode 值、块几何、连接间隔单位等等。只要有人改了头文件却忘了正文，
其中任何一个都可能漂移 - 而这份文档正是编写 Android App 的依据，
所以文档中一个过期的数字就会变成手机上固件无法解释的 bug。

因此本脚本解析头文件，抽出文档本应引用的常量，并断言每一个
都出现在文档里。它刻意保持保守：只检查那些 *本该* 被写下来的值，
并把它们列在表格中报告出来，好让人看清究竟验证了什么。

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
    """头文件里的 `#define NAME value` 对，value 保持原样写法。

        字符串 define 会保留两端的引号，而文档里从来不会带引号引用它们 -
        `#define BLE_CTRL_UUID "494e..."` 必须去掉引号再比较，
        否则每个 UUID 检查都会报出一个虚假的缺口。
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
    """来自 ble_protocol.h 的标量枚举成员（BLE_OP_x = 0x01，BLE_STATE_x = 3...）。"""
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

    # --- 身份 / 版本 --------------------------------------------
        #
        # 下面这两个 expect() 调用才是真正保护文档的东西：它们把文档
        # 与头文件对比，因此版本号一变，正文就不可能被落下。现在刻意
        # 不再有单独的“必须是 2”断言 - 那不过是每次协议修订都要记住的
        # 又一个字面量，而且它并没有抓到任何动态检查抓不到的问题。
    proto = consts.get("BLE_PROTOCOL_VERSION", "?")
    expect(doc, f"版本：**协议 v{proto}**", "header blockquote: protocol version")
    expect(doc, f"协议版本          : {proto}", "constant cheat-sheet: protocol version")
    fw = (f'{consts.get("FW_VERSION_MAJOR", "?")}.'
          f'{consts.get("FW_VERSION_MINOR", "?")}.'
          f'{consts.get("FW_VERSION_PATCH", "?")}')
    expect(doc, f"固件版本          : {fw}", "constant cheat-sheet: firmware version")

    # --- UUID ----------------------------------------------------------
    for name in ("BLE_SERVICE_UUID", "BLE_CTRL_UUID", "BLE_DATA_UUID"):
        value = consts.get(name, "")
        if value:
            expect(doc, value, f"{name}")

    # --- 画布 / 载荷长度 ---------------------------------------
    expect(doc, f'{consts.get("BLE_IMAGE_WIDTH", "?")} x {consts.get("BLE_IMAGE_HEIGHT", "?")}',
           "canvas size (BLE_IMAGE_WIDTH x BLE_IMAGE_HEIGHT)")
    for label, needle in (
        ("2bpp payload length", consts.get("BLE_FMT_2BPP_LEN_HINT", "")),
    ):
        if needle:
            expect(doc, needle, label)
    # 这些长度是算出来的，所以要引用头文件所隐含的数字。
    w = int(consts.get("BLE_IMAGE_WIDTH", "0"))
    h = int(consts.get("BLE_IMAGE_HEIGHT", "0"))
    if w and h:
        expect(doc, str(w * h // 4), f"2bpp payload = {w}x{h}/4")
        expect(doc, str(w * h // 8), f"1bpp payload = {w}x{h}/8")
        expect(doc, str(w // 4), "2bpp bytes per row")
        expect(doc, str(w // 8), "1bpp bytes per row")

    # --- 块几何 -------------------------------------------------
    shift = int(consts.get("BLE_BLOCK_SHIFT_DEFAULT", "0"))
    lo = int(consts.get("BLE_BLOCK_SHIFT_MIN", "0"))
    hi = int(consts.get("BLE_BLOCK_SHIFT_MAX", "0"))
    block = 1 << shift if shift else 0
    expect(doc, str(lo), f"BLE_BLOCK_SHIFT_MIN ({lo} = {1 << lo} B)")
    expect(doc, str(hi), f"BLE_BLOCK_SHIFT_MAX ({hi} = {1 << hi} B)")
    expect(doc, str(consts.get("BLE_MAX_BLOCKS", "?")), "BLE_MAX_BLOCKS")
    expect(doc, f"`1 << 12` = {block} B" if False else f"{block} B",
           f"default block size = 1 << {shift}")
    # 文档所述格式各自的块数量。
    if block:
        expect(doc, str((w * h // 4 + block - 1) // block), "2bpp block count")
        expect(doc, str((w * h // 8 + block - 1) // block), "1bpp block count")
        last = (w * h // 4) - ((w * h // 4) // block) * block
        if last:
            expect(doc, str(last), f"2bpp short final block ({last} B)")

    # --- 分片 / 窗口 --------------------------------------------
    expect(doc, str(consts.get("BLE_DATA_CHUNK_MAX", "?")), "BLE_DATA_CHUNK_MAX")
    expect(doc, str(consts.get("BLE_WINDOW_CHUNKS", "?")), "BLE_WINDOW_CHUNKS")
    expect(doc, str(consts.get("BLE_MTU_PREFERRED", "?")), "BLE_MTU_PREFERRED")

    # --- 连接参数 -----------------------------------------------
    ci_min = int(consts.get("BLE_CONN_MIN_INTERVAL", "0"))
    ci_max = int(consts.get("BLE_CONN_MAX_INTERVAL", "0"))
    expect(doc, f"{ci_min * 1.25:.2f}", f"min connection interval = {ci_min} * 1.25 ms")
    expect(doc, f"{ci_max * 1.25:.0f}", f"max connection interval = {ci_max} * 1.25 ms")
    expect(doc, str(int(consts.get("BLE_LL_TX_OCTETS", "0"))), "BLE_LL_TX_OCTETS")

    # --- 刷新冷却（v3）-------------------------------------------
        # 这个区间在一篇满是数字的文档里只是三个字符的数字，所以
        # 只匹配裸值几乎证明不了什么。改为检查 App 解析的字段名
        # 和它必须实现的命令名，外加文档本应引用的
        # 那个数字。
    gap = consts.get("BLE_REFRESH_MIN_GAP_S", "?")
    expect(doc, "`refresh_min_gap_s`", "GET_INFO trailing field name")
    expect(doc, gap, f"BLE_REFRESH_MIN_GAP_S ({gap} s)")
    for name in ("BLE_OP_SYNC_TIME", "BLE_OP_CONFIRM_REFRESH"):
        expect(doc, name.replace("BLE_OP_", ""), f"{name} named in the doc")

    # --- opcode ----------------------------------------------------------
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
        # 状态表里这个数字是裸写的（"| 9 | `FIXING` | ..."），所以
                # 要匹配那种形式，而不是反引号包裹的值。
        expect(doc, f"| {v} | `{name.replace('BLE_STATE_', '')}`", f"{name} (state row)")

    # --- v1.3.1 新增内容 ------------------------------------------------
        # ERR_STATE 的 END_IMAGE 所携带的那一字节诊断信息，以及 App
        # 必须为它实现的规则。这些名字来自 ble_protocol.h，所以在那边
        # 改名不可能把正文落下。
    expect(doc, "data_hint", "END_IMAGE ERR_STATE hint byte (docs 6.3)")
    hints = [n for n in consts if n.startswith("BLE_HINT_DATA_")]
    if "BLE_HINT_NONE" in consts:
        hints.insert(0, "BLE_HINT_NONE")
    if not hints:
        FAILURES.append("BLE_HINT_* constants not found in ble_protocol.h")
        print("MISS BLE_HINT_*                   hint constants in ble_protocol.h")
    for name in hints:
        short = name.replace("BLE_HINT_", "")
        if short == "NONE":
            continue
        expect(doc, short, f"{name} documented by name")
    expect(doc, "ERR_BUSY", "RETRY_BLOCK busy rule (docs 5.9)")
    expect(doc, "一块一块来", "one-repair-at-a-time rule")

    # --- 绝不能仍然写着 v1 的措辞 -------------------------------
    #
        # 只针对那些若被当成 *当前* 行为来读就会误导实现者的措辞。
        # 版次表的 v1 行是允许描述 v1 的停等窗口的，
        # 所以“窗口流控”不在这个列表里。
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

    # 旧名字允许作为 *历史* 引用出现，但仅当它们周围的文字
        # 说明了这一点 - 一个裸的 `EVT_WINDOW_ACK` 会把 App 作者
        # 引向错误的方向。
    #
        # “周围”指的是所在的整个章节，而不是一个字符窗口：那张
        # v1 -> v2 的对比表有九行那么长，所以解释它的标记在标题里，
        # 而不在任何合理的字符距离之内。
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
