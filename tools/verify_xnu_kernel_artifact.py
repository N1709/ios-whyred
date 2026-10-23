#!/usr/bin/env python3
"""Verify a file is a real XNU Mach-O kernel artifact — not shim/out/Image.

Exits 0 only if Mach-O magic + size floor match. Never claims boot-ready.
"""
from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

# MH_MAGIC_64 / MH_CIGAM_64 / FAT_MAGIC / FAT_CIGAM (little-endian file bytes)
MACHO_MAGICS = {
    b"\xcf\xfa\xed\xfe",
    b"\xfe\xed\xfa\xcf",
    b"\xca\xfe\xba\xbe",
    b"\xbe\xba\xfe\xca",
}
ARM64_IMAGE_MAGIC = b"ARM\x64"  # Linux ARM64 Image header at offset 56
MIN_SIZE = 1_000_000


def classify(path: Path) -> tuple[bool, list[str]]:
    data = path.read_bytes()
    notes: list[str] = []
    size = len(data)
    notes.append(f"size={size}")
    notes.append(f"sha256={hashlib.sha256(data).hexdigest()}")
    if size >= 60 and data[56:60] == ARM64_IMAGE_MAGIC:
        notes.append("FAIL: Linux ARM64 Image magic at +56 (shim/out/Image shape) — not XNU")
        return False, notes
    if size < 4:
        notes.append("FAIL: too small")
        return False, notes
    magic = data[:4]
    if magic not in MACHO_MAGICS:
        notes.append(f"FAIL: not Mach-O (magic={magic.hex()})")
        return False, notes
    notes.append(f"Mach-O magic OK ({magic.hex()})")
    if size < MIN_SIZE:
        notes.append(f"FAIL: size < {MIN_SIZE} (unlikely full XNU kernel)")
        return False, notes
    name = path.name.lower()
    if name == "image" or name.endswith("/image"):
        notes.append("FAIL: filename Image — refuse shim mislabel")
        return False, notes
    if not name.startswith("kernel."):
        notes.append("WARN: filename does not start with kernel. (still Mach-O)")
    notes.append("PASS: structurally a Mach-O kernel-sized artifact")
    return True, notes


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("path", type=Path)
    ap.add_argument("--expect-name", help="exact basename required, e.g. kernel.development.emerald")
    ns = ap.parse_args(argv)
    if not ns.path.is_file():
        print(f"ABSENT {ns.path}")
        return 2
    if ns.expect_name and ns.path.name != ns.expect_name:
        print(f"FAIL: expected basename {ns.expect_name}, got {ns.path.name}")
        return 1
    ok, notes = classify(ns.path)
    for n in notes:
        print(n)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
