# progres.md — live progress board

Update this file at every milestone, before handing off, and whenever a boot
result changes the picture. Keep it short: facts and next actions only. The
long reasoning belongs in `history/`.

Last updated: 2026-11-22.

## What works

| Feature | State |
| --- | --- |
| Boot (`fastboot boot`) | **Works** |
| Display / framebuffer | **Works** |
| Storage (eMMC + HFSX root) | **Works** |
| Console log ring | **Works** |
| Kernel log / panic dump | **Works** |
| Touch (probe) | **Works** |
| Touch (as HID input) | No |
| Buttons | Added (0208), not booted |
| RTC / date | Added (0209), not booted |
| eMMC ADMA2 reads | Added (0210), not booted |
| Panel 60 Hz / present timing | Added (0207), not booted |
| GPU / Metal | No |
| Wi-Fi | ? |
| Bluetooth | ? |
| Audio | ? |
| Cellular | ? |
| Charging | ? |
| iOS userland (launchd → SpringBoard) | No (code signing) |

Legend: **Works** = verified on the phone; No = not implemented or known
blocked; ? = not tested yet.

## Goal

Boot Apple's iOS 16.7.16 (iPhone X userland) on the Redmi Note 5 (`whyred`,
SDM636) from `fastboot boot`, running the kernel's own display and storage
drivers, and make the Synaptics touch controller work as real HID input.

## Where we are

| Area | State |
| --- | --- |
| Boot | `fastboot boot` of our shim → XNU runs. Boot/modem/persist/recovery are never flashed |
| CPU / memory | 4× Gold A73, 16K pages, GICv3, timers — **works** |
| eMMC | SDCC v5, 8-bit high-speed, 122,142,720 sectors, GPT parsed — **works** |
| Root volume | HFSX `userdata` mounts read-write as `emmcroot0` — **works** |
| Display | Kernel framebuffer + 4-bar test visible on panel — **works** |
| Console log | Ring flusher on SYSTEM (`SANTONILOG1`), pull with `../tools/sweet-log-pull.sh` — **works** |
| iOS userland | launchd starts, then dies at code signing (`CS_KILLED`) — **blocked** |
| Touch | DSX at `0x20` probed, F12 programmed, tap poll exists — **not in HID yet** |

## New fixes 0207-0210 (2026-11-22, not booted)

Four small SDM660 fixes from `pcorezzola/lavender-ios16` are now whyred
patches. They are local code only: no CI build and no boot yet.

| Patch | What it adds | Boot-arg |
| --- | --- | --- |
| 0207 | Display default 60 Hz, vsync gated on flush ticks, per-swap telemetry and `present()` timing | `sweetfb_hz`, `sweetfblog` |
| 0208 | PM660/PM660L keys over SPMI arbiter v3, started from `santoni_sdhci.c` | `sweetkeys=0` disables |
| 0209 | PM660 RTC through the same arbiter; skips the 30 s IORTC wait; calendar fallback | `sweetrtc`, `sweetrtcoff` |
| 0210 | SDCC v5 exact PIO/cache state, post-write DAT0/CMD13 wait, DMA diagnostics, ADMA2 reads with PIO fallback | `sweetemmc_dma=0`, `sweetemmc_speed=1`, `sweetemmc_dmatest=1` |

All 216 patches apply on the whyred series tip; `drivers/` mirrors are
regenerated. The next boot must confirm the banner `driver 0210`, the
ADMA2 self-test, `SWEET KEYS:` lines, `SWEET RTC:` raw/offset and the
`SWEET FB: present ... us` budget.

## Latest ring (patch 0205)

* 0205 boots; the raw scan shows the part **ignores the address phase** after
  the first read and streams its whole register map.
* In that stream the PDT records are back to back and ascending: F34
  `27 00 09 00 21 34`, **F01 `2f 26 0d 04 01 01`**, F12
  `4c 00 13 06 01 12`, end marker, then the same 536 bytes again. The
  `00 06 0d 11` literal does not exist anywhere in it.
* The real F01 command base is therefore **0x26**, not `0x06`. 0196/0205
  wrote the reset command `0x01` to `0x06`, which is F12's **data** register,
  so the controller never left wakeup-gesture/suspend.
* The walk's garbage `dsx_f01_ctrl=0x83` also sent the F01 control write to
  `0x0083` (`ack` there is meaningless).
* `tools/dsx-pdt.py` now reports the F34/F01/F12 triplet and the stream
  phase.

Image 0205: `shim-boot-touch-f01-scan.img` (`SWEETREBOOT=0`),
sha256 `a0ad381cc88396de3a8f1ce44d35e5ee0becf06e4f8dbf0962856218737593f6`,
6,119,424 B, CI run `44071902075`.

Image 0206: `shim-boot-touch-pdt-reset.img` (`SWEETREBOOT=0`),
sha256 `4b55e7916dbda67c8835ecfefa105fdf4177862dd7954b1838a56636c9a42139`,
6,119,424 B, CI run `44506155564` (kernel `23e59b3`).

## Next actions (in order)

1. **Build 0207-0210 in CI**, then boot the new image and check
   `WHYRED SDHCI: driver 0210`, the ADMA2 self-test, `SWEET KEYS:`,
   `SWEET RTC:` and `SWEET FB: present ... us`.
2. **Boot 0206** (`shim-boot-touch-pdt-reset.img`) and read the ring for
   `pdt stream F01 ... cmd 26`, `addr test`, `address phase`, `f01 cmd
   0x0026 <- 0x01` and `f12 ctrl28 <- 0x1f`.
3. If the address phase works, confirm `f12 ctrl28 <- 0x1f ack` with a
   read-back, then run the tap test.
4. If it still streams, the next step is documented in `history/2026-11.md`.
5. Decode X/Y and wire `SweetHIDTouch::touch(finger, down, x, y)`.
6. Keep `SWEETREBOOT=0` while debugging so the phone stays in place.


## Blockers / unknowns

* No finger-down F12 samples yet → the 2D report layout is inferred.
* Whether the F01 reset at `0x26` actually leaves LPWG is the open question;
  the 0206 boot answers it.
* No local `unixnu build`; every kernel iteration costs a CI run.

## Key files

| Path | Why |
| --- | --- |
| `STATUS.md` | Longer status, artifact table |
| `touch.md` | DSX probe, F12 setup, band map, boot args |
| `history/` | Full engineering log, per month |
| `../xnu/patches/0206-whyred-touch-pdt-record-reset.patch` | Latest patch (0206) |
| `../tools/build-touch-diag.sh` | Pack a diag image from a CI kernel |
| `../tools/sweet-log-pull.sh` | Pull the ring from SYSTEM |

## Device / workflow facts

* Enter fastboot: hold Power ~12 s, wait 5 s, then Power + Volume Down.
* Enter recovery (OrangeFox): Power + Volume Up. adb serial `611026fa`.
* Local folder: `ios-whyred` (renamed from `ios-whyred2` on 2026-11-17).
  The GitHub repo is still `N1709/ios-whyred`; rename it on GitHub, then
  update this URL.
* Push target: `ssh://git@ssh.github.com:443/N1709/ios-whyred.git`,
  branch `whyred`. Never push to `main`.
