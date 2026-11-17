#!/usr/bin/env python3
"""Reassemble the DSX raw register dump from a pulled console ring.

Patch 0205 makes the probe read 0x0000..0x0200 in 61-byte chunks and print
each chunk as

    WHYRED TOUCH: dsx f01scan [0x00dd] 4c 00 13 06 01 12 |L.....|

This tool rebuilds that byte space from one or more ring files, reports
coverage, finds the F01 function descriptor (00 06 0d 11) at any offset and
prints the neighbouring registers, so the real PDT start can be picked
without guessing from the chunk boundaries.

    tools/dsx-pdt.py RING [RING ...] [--dump 0x00d0 0x0100]
"""

import argparse
import re
import sys

ROW = re.compile(r"dsx f01scan \[0x([0-9a-fA-F]{4})\]\s*((?:[0-9a-fA-F]{2}\s*)*)")

F01 = bytes((0x00, 0x06, 0x0d, 0x11))
# RMI4 functions whose descriptor starts with these data/control bases are
# worth printing next to a hit; the number itself is just informative.
FUNC_NAMES = {
    0x01: "F01 device control",
    0x11: "F11 2D touch",
    0x12: "F12 2D touch",
    0x1A: "F1A 0D button",
    0x34: "F34 flash",
    0x54: "F54 analog",
}


def parse(paths):
    data = bytearray()
    seen = {}
    for path in paths:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                m = ROW.search(line)
                if not m:
                    continue
                addr = int(m.group(1), 16)
                raw = bytes.fromhex(m.group(2))
                for i, value in enumerate(raw):
                    pos = addr + i
                    if pos in seen and seen[pos] != value:
                        print(f"conflict at 0x{pos:04x}: "
                              f"{seen[pos]:02x} vs {value:02x}", file=sys.stderr)
                    seen[pos] = value
                if addr + len(raw) > len(data):
                    data.extend(b"\xff" * (addr + len(raw) - len(data)))
                data[addr:addr + len(raw)] = raw
    return bytes(data), seen


def dump(blob, start, end):
    for base in range(start, end, 16):
        row = blob[base:base + 16]
        hexs = " ".join(f"{b:02x}" for b in row)
        text = "".join(chr(b) if 0x20 <= b < 0x7f else "." for b in row)
        marker = ""
        hit = blob.find(F01, base, base + 16)
        if hit >= 0:
            marker = f"  <-- F01 at 0x{hit:04x}"
        print(f"{base:04x}: {hexs:<47}  {text}{marker}")


def records(blob, start, count=24):
    """Print six-byte PDT entries from start, stopping at padding/empty."""
    for n in range(count):
        addr = start + n * 6
        entry = blob[addr:addr + 6]
        if len(entry) < 6:
            break
        if entry == b"\x00\x00\x00\x00\x00\x00" or entry == b"\xff" * 6:
            print(f"  {addr:04x}: <end>")
            break
        query, cmd, ctrl, data, ver, raw = entry
        fn = raw & 0x3f
        name = FUNC_NAMES.get(fn, "")
        print(f"  {addr:04x}: q=0x{query:02x} c=0x{cmd:02x} "
              f"ctl=0x{ctrl:02x} d=0x{data:02x} v=0x{ver:02x} "
              f"fn=0x{fn:02x}  {name}")
        if fn == 0x00 or raw == 0xff:
            break


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rings", nargs="+")
    ap.add_argument("--dump", nargs=2, metavar=("START", "END"),
                    type=lambda s: int(s, 0),
                    help="hex dump of a range, e.g. --dump 0xd0 0x110")
    args = ap.parse_args()

    blob, seen = parse(args.rings)
    if not seen:
        print("no 'dsx f01scan' rows in these rings", file=sys.stderr)
        return 1

    holes = [a for a in range(min(seen), max(seen) + 1) if a not in seen]
    print(f"coverage: 0x{min(seen):04x}..0x{max(seen):04x}, "
          f"{len(seen)} bytes, {len(holes)} missing")
    if holes:
        print("missing: " + " ".join(f"0x{a:04x}" for a in holes[:16])
              + (" ..." if len(holes) > 16 else ""))

    hits = []
    pos = blob.find(F01)
    while pos >= 0:
        hits.append(pos)
        pos = blob.find(F01, pos + 1)
    if not hits:
        print("no 00 06 0d 11 record")
    for pos in hits:
        print(f"\nF01 at 0x{pos:04x}:")
        print(f"  q=0x{blob[pos]:02x} c=0x{blob[pos + 1]:02x} "
              f"ctl=0x{blob[pos + 2]:02x} d=0x{blob[pos + 3]:02x} "
              f"v=0x{blob[pos + 4]:02x} fn=0x{blob[pos + 5] & 0x3f:02x}")
        print("  PDT from here:")
        records(blob, pos)

    if args.dump:
        start, end = args.dump
        data_end = max(seen) + 1
        end = min(end, data_end)
        print()
        dump(blob, start, end)
    return 0 if hits else 2


if __name__ == "__main__":
    sys.exit(main())
