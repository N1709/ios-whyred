#!/usr/bin/env python3
"""Rebuild 0082-emerald-machine-config.patch against xnu-8796.141.3 + patches < 0082."""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

XNU = Path(r"C:\ios-emerald\downloads\xnu-8796.141.3")
PATCH_DIR = Path(r"C:\ios-emerald\ios-sweet\xnu\patches")
OUT = PATCH_DIR / "0082-emerald-machine-config.patch"


def run(args: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(args, check=False, capture_output=True, text=True, encoding="utf-8", **kw)


def main() -> int:
    # Fresh tree at tagged commit
    run(["git", "-C", str(XNU), "reset", "--hard", "1b191cb58250d0705d8a51287127505aa4bc0789"])
    run(["git", "-C", str(XNU), "clean", "-fd"])

    patches = sorted(
        p
        for p in PATCH_DIR.glob("*.patch")
        if p.name < "0082-emerald-machine-config.patch"
    )
    applied = 0
    skipped = 0
    for p in patches:
        chk = run(["git", "-C", str(XNU), "apply", "--check", "--whitespace=nowarn", str(p)])
        if chk.returncode != 0:
            print(f"SKIP {p.name}: {chk.stderr.strip()[:120]}")
            skipped += 1
            continue
        ap = run(["git", "-C", str(XNU), "apply", "--whitespace=nowarn", str(p)])
        if ap.returncode != 0:
            print(f"FAIL apply {p.name}: {ap.stderr.strip()[:200]}")
            return 1
        applied += 1
    print(f"pre_patches applied={applied} skipped={skipped}")

    # Commit pre-0082 baseline
    run(["git", "-C", str(XNU), "add", "-A"])
    run(
        ["git", "-C", str(XNU), "commit", "-m", "pre-0082 sweet series"],
        env={
            **dict(**{k: v for k, v in __import__("os").environ.items()}),
            "GIT_AUTHOR_NAME": "emerald",
            "GIT_AUTHOR_EMAIL": "emerald@local",
            "GIT_COMMITTER_NAME": "emerald",
            "GIT_COMMITTER_EMAIL": "emerald@local",
        },
    )

    # --- EMERALD edits ---
    p = XNU / "makedefs/MakeInc.def"
    t = p.read_text(encoding="utf-8")
    if "SWEET EMERALD" not in t:
        if "VMAPPLE SWEET\n" not in t:
            raise SystemExit("expected SWEET in SUPPORTED_ARM64_MACHINE_CONFIGS")
        t = t.replace(
            "SUPPORTED_ARM64_MACHINE_CONFIGS = BCM2837 T6000 T8101 T8103 VMAPPLE SWEET\n",
            "SUPPORTED_ARM64_MACHINE_CONFIGS = BCM2837 T6000 T8101 T8103 VMAPPLE SWEET EMERALD\n",
        )
    old_flags = (
        "MACHINE_FLAGS_ARM64_SWEET = -DARM64_BOARD_CONFIG_SWEET                "
        "-march=armv8.2-a+crypto\n"
    )
    if "MACHINE_FLAGS_ARM64_EMERALD" not in t:
        if old_flags not in t:
            raise SystemExit("SWEET MACHINE_FLAGS missing")
        t = t.replace(
            old_flags,
            old_flags
            + "MACHINE_FLAGS_ARM64_EMERALD = -DARM64_BOARD_CONFIG_EMERALD            "
            "-march=armv8.2-a+crypto\n",
        )
    old_gate = (
        "# sweet's Cortex-A76/A55 have no pointer authentication: stay plain arm64\n"
        "ifneq ($(CURRENT_MACHINE_CONFIG),SWEET)\n"
        "export ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG := arm64e\n"
        "endif\n"
    )
    new_gate = (
        "# sweet/emerald Cortex-A76/A55 have no pointer authentication: stay plain arm64\n"
        "ifeq ($(filter $(CURRENT_MACHINE_CONFIG),SWEET EMERALD),)\n"
        "export ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG := arm64e\n"
        "endif\n"
    )
    if "SWEET EMERALD)," not in t:
        if old_gate not in t:
            raise SystemExit("SWEET arm64e gate missing")
        t = t.replace(old_gate, new_gate)
    p.write_text(t, encoding="utf-8", newline="\n")

    p = XNU / "pexpert/pexpert/arm64/Makefile"
    t = p.read_text(encoding="utf-8")
    if "EMERALD.h" not in t:
        if "\tSWEET.h\n" not in t and "\tSWEET.h \\\n" not in t:
            # already may end without newline variants
            raise SystemExit(f"SWEET.h not in Makefile: {t!r}"[:200])
        if "\tSWEET.h\n" in t:
            t = t.replace("\tSWEET.h\n", "\tSWEET.h \\\n\tEMERALD.h\n")
        p.write_text(t, encoding="utf-8", newline="\n")

    (XNU / "pexpert/pexpert/arm64/EMERALD.h").write_text(
        """/*
 * Board config for Redmi Note 13 Pro 4G ("emerald", MediaTek MT6789 /
 * Helio G99 Ultra class): 2x Cortex-A76 + 6x Cortex-A55, ARMv8.2, no PAC.
 *
 * Reuses the generic-ARM SWEET bring-up (GICv3 Group 1, 16K pages, no
 * APPLE_ARM64_ARCH_FAMILY). Timer INTID differs from Qualcomm sweet:
 * G0-R-2 DT arm,armv8-timer PPI 11 -> INTID 27 (not sweet's INTID 19).
 */

#ifndef _PEXPERT_ARM64_EMERALD_H
#define _PEXPERT_ARM64_EMERALD_H

#define EMERALD                   1

/* Activate existing #if defined(SWEET) paths without deleting SWEET board. */
#define SWEET                     1

#include <pexpert/arm64/SWEET.h>

/* Override sweet Qualcomm PPI numbering with emerald G0-R-2 values. */
#undef SWEET_VTIMER_INTID
#define SWEET_VTIMER_INTID        27
#define EMERALD_VTIMER_INTID      27
#define EMERALD_PTIMER_INTID      30

#endif /* ! _PEXPERT_ARM64_EMERALD_H */
""",
        encoding="utf-8",
        newline="\n",
    )

    p = XNU / "pexpert/pexpert/arm64/board_config.h"
    t = p.read_text(encoding="utf-8")
    if "ARM64_BOARD_CONFIG_EMERALD" not in t:
        key = "#endif  /* ARM64_BOARD_CONFIG_SWEET */\n"
        if key not in t:
            raise SystemExit("SWEET board_config endif missing")
        t = t.replace(
            key,
            key
            + """
#ifdef ARM64_BOARD_CONFIG_EMERALD
#include <pexpert/arm64/EMERALD.h>

#define MAX_L2_CLINE                   6
#define MAX_CPUS                       8
#define MAX_CPU_CLUSTERS               1

#define CORE_NCTRS                     8 /* Placeholder; KPC is not enabled for this target */

#endif  /* ARM64_BOARD_CONFIG_EMERALD */

""",
        )
        p.write_text(t, encoding="utf-8", newline="\n")

    p = XNU / "iokit/KernelConfigTables.cpp"
    t = p.read_text(encoding="utf-8")
    if "xiaomi,sweet" not in t:
        raise SystemExit("xiaomi,sweet missing — 0016 did not apply?")
    if "xiaomi,emerald" not in t:
        # Broaden ifdef and append emerald entry before the sweet block's #endif
        t = t.replace(
            "#if defined(ARM64_BOARD_CONFIG_SWEET)\n",
            "#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(ARM64_BOARD_CONFIG_EMERALD)\n",
            1,
        )
        marker = '    "     \'IONameMatch\'     = \'xiaomi,sweet\';"\n'
        idx = t.find(marker)
        if idx < 0:
            raise SystemExit("sweet IONameMatch not found")
        # find the #endif after this sweet entry (first #endif after marker)
        endif = t.find("#endif\n", idx)
        if endif < 0:
            raise SystemExit("endif after sweet entry not found")
        insert = (
            "#if defined(ARM64_BOARD_CONFIG_EMERALD)\n"
            '    "   {"\n'
            '    "     \'IOClass\'         = SweetPlatformExpert;"\n'
            '    "     \'IOProviderClass\' = IOPlatformExpertDevice;"\n'
            '    "     \'IONameMatch\'     = \'xiaomi,emerald\';"\n'
            '    "     \'IOProbeScore\'    = 10000:32;"\n'
            '    "   },"\n'
            "#endif\n"
        )
        t = t[:endif] + insert + t[endif:]
        p.write_text(t, encoding="utf-8", newline="\n")

    # Stage so new files (EMERALD.h) appear in the diff
    run(["git", "-C", str(XNU), "add", "-A"])
    diff = run(["git", "-C", str(XNU), "diff", "--cached"])
    if diff.returncode != 0 or "EMERALD" not in diff.stdout:
        raise SystemExit(f"bad diff: {diff.stderr or 'no EMERALD in staged diff'}")
    if "pexpert/pexpert/arm64/EMERALD.h" not in diff.stdout:
        raise SystemExit("EMERALD.h missing from staged diff — refuse incomplete 0082")
    header = (
        "From: emerald port\n"
        "Subject: [PATCH] emerald: ARM64_BOARD_CONFIG_EMERALD alongside SWEET\n"
        "\n"
        "Adds EMERALD machine config for Redmi Note 13 Pro 4G (MT6789).\n"
        "Must apply after the sweet series (<0082) on xnu-8796.141.3 (macOS 13.5).\n"
        "\n"
        "---\n"
    )
    OUT.write_text(header + diff.stdout, encoding="utf-8", newline="\n")
    print(f"wrote {OUT} size={OUT.stat().st_size}")

    # Round-trip: reset to pre-0082 commit and apply
    run(["git", "-C", str(XNU), "reset", "--hard", "HEAD"])
    run(["git", "-C", str(XNU), "clean", "-fd"])
    chk = run(["git", "-C", str(XNU), "apply", "--check", str(OUT)])
    if chk.returncode != 0:
        print(chk.stderr)
        return 1
    ap = run(["git", "-C", str(XNU), "apply", str(OUT)])
    if ap.returncode != 0:
        print(ap.stderr)
        return 1
    text = (XNU / "makedefs/MakeInc.def").read_text(encoding="utf-8")
    assert "VMAPPLE SWEET EMERALD" in text
    assert "MACHINE_FLAGS_ARM64_EMERALD" in text
    assert (XNU / "pexpert/pexpert/arm64/EMERALD.h").is_file()
    emerald_h = (XNU / "pexpert/pexpert/arm64/EMERALD.h").read_text(encoding="utf-8")
    assert "SWEET_VTIMER_INTID        27" in emerald_h
    assert "xiaomi,emerald" in (XNU / "iokit/KernelConfigTables.cpp").read_text(
        encoding="utf-8"
    )
    print(
        "ROUNDTRIP_OK: MACHINE_CONFIG=EMERALD applicable on xnu-8796.141.3 "
        "after sweet patches <0082"
    )
    print(
        "CI_BUILD_CMD: KERNEL_CONFIG=DEVELOPMENT ARCH_CONFIG=ARM64 "
        "MACHINE_CONFIG=EMERALD MACOS_VERSION=13.5 bash build.sh"
    )
    print("EXPECTED_ARTIFACT: kernel.development.emerald")
    return 0


if __name__ == "__main__":
    sys.exit(main())
