#!/usr/bin/env python3
"""Reassemble the DSX register map from a pulled console ring.

Patch 0205 read 0x0000..0x0200 in 61-byte chunks; patch 0206 reads the same
space in one 0x200-byte transfer and prints it in 64-byte rows. Both use

    WHYRED TOUCH: dsx f01scan [0x00dd] 4c 00 13 06 01 12 |L.....|

This tool rebuilds that byte space from one or more ring files, reports
coverage, and locates the PDT in either layout. A running part streams its
register map in walk order, so the records sit back to back:

    27 00 09 00 21 34   F34
    2f 26 0d 04 01 01   F01  query 0x2f cmd 0x26 ctrl 0x0d data 0x04
    4c 00 13 06 01 12   F12  query 0x4c cmd 0x00 ctrl 0x13 data 0x06

When the address phase works the vendor walk sees the same records six bytes
apart in the other direction (F34, then F01 below it, then F12). The report
names the phase and prints the bases the probe must use.

    tools/dsx-pdt.py RING [RING ...] [--dump 0x00d0 0x0100]
"""

import argparse
import re
import sys

ROW = re.compile(r"dsx f01scan \[0x([0-9a-fA-F]{4})\]\s*((?:[0-9a-fA-F]{2}\s*)*)")

F01_STD = bytes((0x00, 0x06, 0x0d, 0x11))
F01 = F01_STD
F34 = bytes((0x27, 0x00, 0x09, 0x00, 0x21, 0x34))
F12 = bytes((0x4c, 0x00, 0x13, 0x06, 0x01, 0x12))
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


def find_near(blob, needle, around, radius=24):
    """First exact six-byte record within radius of an offset."""
    lo = max(0, around - radius)
    hi = min(len(blob) - 6, around + radius)
    for i in range(lo, hi + 1):
        if blob[i:i + 6] == needle:
            return i
    return None


def find_triplet(blob):
    """Locate F34/F01/F12 in either layout.

    Returns (f34, f01, f12, phase) or None. While the part streams its map
    the records are back to back and ascending; when the address phase works
    the vendor walk sees F12 below F01 below F34.
    """
    pos = 0
    while True:
        f12 = blob.find(F12, pos)
        if f12 < 0:
            return None
        f01 = None
        for i in range(max(0, f12 - 24), min(len(blob) - 6, f12 + 24) + 1):
            entry = blob[i:i + 6]
            if (entry[5] & 0x3f) == 0x01 and entry[2] == 0x0d:
                f01 = i
                break
        if f01 is not None:
            f34 = find_near(blob, F34, f01, 24)
            phase = "walk order" if f01 < f12 else "address order"
            return f34, f01, f12, phase
        pos = f12 + 1


def print_triplet(blob, f34, f01, f12, phase):
    def rec(off, name):
        q, c, ctl, d, ver, raw = blob[off:off + 6]
        fn = raw & 0x3f
        print(f"  {name} at 0x{off:04x}: q=0x{q:02x} cmd=0x{c:02x} "
              f"ctl=0x{ctl:02x} data=0x{d:02x} v=0x{ver:02x} "
              f"fn=0x{fn:02x}")

    print(f"\nPDT triplet in {phase}:")
    rec(f01, "F01")
    rec(f12, "F12")
    if f34 is not None:
        rec(f34, "F34")
    print(f"  use: F01 cmd=0x{blob[f01 + 1]:02x} "
          f"ctrl=0x{blob[f01 + 2]:02x} data=0x{blob[f01 + 3]:02x}; "
          f"F12 query=0x{blob[f12]:02x} ctrl=0x{blob[f12 + 2]:02x} "
          f"data=0x{blob[f12 + 3]:02x}")


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
        print("no 00 06 0d 11 record (this unit reports 2f 26 0d 04 01 01)")
    for pos in hits:
        print(f"\nF01 at 0x{pos:04x}:")
        print(f"  q=0x{blob[pos]:02x} c=0x{blob[pos + 1]:02x} "
              f"ctl=0x{blob[pos + 2]:02x} d=0x{blob[pos + 3]:02x} "
              f"v=0x{blob[pos + 4]:02x} fn=0x{blob[pos + 5] & 0x3f:02x}")
        print("  PDT from here:")
        records(blob, pos)

    triplet = find_triplet(blob)
    if triplet:
        print_triplet(blob, *triplet)
    else:
        print("no F34/F01/F12 triplet in this dump")

    if args.dump:
        start, end = args.dump
        data_end = max(seen) + 1
        end = min(end, data_end)
        print()
        dump(blob, start, end)
    return 0 if hits or triplet else 2


if __name__ == "__main__":
    sys.exit(main())
