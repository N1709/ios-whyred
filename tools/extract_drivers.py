#!/usr/bin/env python3
"""Mirror the driver sources that live inside the XNU patch series.

The build only ever sees the patched XNU tree, so the canonical copy of a
driver is the patch in xnu/patches/. That makes drivers/ look empty even when
a driver exists. This tool reconstructs each in-tree file from the patches and
stores a read-only mirror under drivers/<domain>/, so the code can be read,
grepped and reviewed without checking out XNU.

    tools/extract_drivers.py            rewrite the mirrors
    tools/extract_drivers.py --check    fail if a mirror is stale

Never edit a mirror: change the patch and re-run the tool. tests/run_tests.sh
runs --check so a patch and its mirror cannot drift apart.
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PATCH_DIR = ROOT / "xnu" / "patches"

# domain -> in-tree files that make up that driver
DRIVERS = {
    "storage": ["bsd/dev/santoni_sdhci.c"],
    "display": [
        "iokit/Kernel/arm/SweetFramebuffer.cpp",
        "iokit/Kernel/arm/SweetSurface.cpp",
    ],
    "touch": [
        "bsd/dev/sweet_touch.c",
        "bsd/dev/whyred_touch.c",
        "iokit/Kernel/arm/hid/SweetHID.cpp",
    ],
    "wifi": [
        "iokit/Kernel/arm/TopazModem.cpp",
        "bsd/dev/whyred_wlan.c",
    ],
    "modem": ["bsd/dev/whyred_modem.c"],
    "gpu": [
        "bsd/dev/whyred_gpu.c",
        "iokit/Kernel/arm/WhyredGPU.cpp",
    ],
    "audio": ["bsd/dev/whyred_audio.c"],
    "power": [
        "iokit/Kernel/arm/WhyredBattery.cpp",
        "bsd/dev/whyred_power.c",
    ],
}


def patches():
    found = sorted(PATCH_DIR.glob("*.patch"))
    if not found:
        sys.exit(f"no patches under {PATCH_DIR}")
    return found


def includes_for(path, series):
    """Paths to include: the file plus any name it was renamed from.

    A rename patch reads the old path and writes the new one, so filtering
    only the new path leaves git apply without the source file.
    """
    names = [path]
    changed = True
    while changed:
        changed = False
        for patch in series:
            src = None
            for line in patch.read_text(errors="replace").splitlines():
                if line.startswith("rename from "):
                    src = line[len("rename from "):].strip()
                elif line.startswith("rename to ") and src:
                    dst = line[len("rename to "):].strip()
                    if dst in names and src not in names:
                        names.append(src)
                        changed = True
    return names


def extract(path, work, series):
    """Reconstruct one in-tree file by applying every patch that touches it."""
    work.mkdir(parents=True, exist_ok=True)
    cmd = ["git", "apply"]
    for name in includes_for(path, series):
        cmd.append(f"--include={name}")
    cmd += ["--whitespace=nowarn"] + [str(p) for p in series]
    proc = subprocess.run(cmd, cwd=work, capture_output=True, text=True)
    if proc.returncode != 0:
        print(proc.stderr.strip(), file=sys.stderr)
        sys.exit(f"git apply failed for {path}")
    out = work / path
    if not out.exists():
        sys.exit(f"{path} did not appear after the patch series")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="only compare; exit 1 if a mirror is stale")
    args = ap.parse_args()
    series = patches()
    stale = []
    tmp = Path(tempfile.mkdtemp(prefix="drv-"))
    try:
        for domain, files in DRIVERS.items():
            dest_dir = ROOT / "drivers" / domain
            dest_dir.mkdir(parents=True, exist_ok=True)
            for path in files:
                src = extract(path, tmp / domain, series)
                dest = dest_dir / Path(path).name
                if args.check:
                    if not dest.exists() or dest.read_bytes() != src.read_bytes():
                        stale.append(str(dest.relative_to(ROOT)))
                    continue
                shutil.copyfile(src, dest)
                print(f"{dest.relative_to(ROOT)} <- {path}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if args.check:
        if stale:
            print("stale driver mirrors (run tools/extract_drivers.py):",
                  file=sys.stderr)
            for s in stale:
                print(f"  {s}", file=sys.stderr)
            return 1
        print("ok   drivers/ mirrors match the patch series")
    return 0


if __name__ == "__main__":
    sys.exit(main())
