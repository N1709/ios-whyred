# ios-whyred

**Apple's iOS 16 on a Redmi Note 5 / Note 5 Pro.**

A port of XNU 8796 (`xnu-8796.141.3`) to the Xiaomi Redmi Note 5
(`whyred`, Qualcomm SDM636), running the unmodified iOS 16.7.16 (20H392)
userland of the iPhone X: launchd, backboardd, SpringBoard.

> Experimental. The iOS root lives inside the existing `userdata` partition
> and **erases Android's user data**. Nothing is flashed: the kernel is
> started from RAM with `fastboot boot`.

## What works

| Area | State |
| --- | --- |
| Boot | **Works** — `fastboot boot` of the shim, boot partitions untouched |
| CPU / memory | **Works** — 4× Cortex-A73 Gold, 16K pages, GICv3, timers |
| Storage | **Works** — eMMC 5.1 at 8-bit high-speed, GPT, HFSX root read-write |
| Display | **Works** — kernel framebuffer, four colour bars visible on the panel |
| Console log | **Works** — persistent ring on SYSTEM, pulled from recovery |
| Touch | Probe finds the streamed PDT and resets at the real F01 base; F12 reporting awaits the 0206 ring, no HID input |
| iOS userland | `launchd` starts, then dies at code signing (`CS_KILLED`) |
| GPU / Wi-Fi / Bluetooth / audio / cellular / charging | Not implemented |

Patches `0207`-`0210` add a 60 Hz display default with `present()` timing,
PM660/PM660L keys, the PM660 RTC with a calendar fallback, and ADMA2 eMMC
reads with a PIO fallback. They apply on the series tip but have not been
built or booted yet.

## Quick start

```sh
# 1. kernel comes from CI (one run per patch, ~10-15 min)
curl -s "https://api.github.com/repos/N1709/ios-whyred/actions/runs?branch=whyred&per_page=1"

# 2. pack a boot image from a finished run
env SWEETEARLY=1000 SWEETI2C=0 SWEETRESET=0 OUT_IMAGE=images/shim-boot.img \
    bash tools/build-touch-diag.sh <run-id>

# 3. boot it (never flash)
fastboot boot images/shim-boot.img

# 4. pull the log: OrangeFox, then
bash tools/sweet-log-pull.sh /tmp/ring.txt
```

## Documentation

| File | What |
| --- | --- |
| [`docs/progres.md`](docs/progres.md) | Live board: what works, latest boot, next actions |
| [`rules.md`](rules.md) | Hard rules for AI agents |
| [`docs/STATUS.md`](docs/STATUS.md) | Status, boot result, image hashes |
| [`docs/touch.md`](docs/touch.md) | Touch probe, F12 setup, panel band map |
| [`docs/build-and-boot.md`](docs/build-and-boot.md) | Boot path, build steps, repository layout |
| [`docs/hardware.md`](docs/hardware.md) | SoC, memory, storage, panel, pin facts |
| [`docs/patches.md`](docs/patches.md) | XNU patch series |
| [`docs/roadmap.md`](docs/roadmap.md) | What is left |
| [`docs/history/`](docs/history/) | Engineering log, one file per month |

## Credits

Based on [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet),
which did the hard part: XNU on a Qualcomm phone with the iPhone X userland,
and the lavender port this whyred target is derived from. Built on Apple's
open-source releases (xnu, IOHIDFamily, libpthread, hfs),
[BearSSL](https://bearssl.org),
[blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build),
[ipsw](https://github.com/blacktop/ipsw), Qualcomm/Xiaomi's published Linux
kernel sources, and the hardware work in
[MakrSas/topaz-woa](https://github.com/MakrSas/topaz-woa). iOS itself is
Apple's and is not distributed here.
