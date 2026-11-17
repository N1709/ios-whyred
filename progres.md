# progres.md — live progress board

Update this file at every milestone, before handing off, and whenever a boot
result changes the picture. Keep it short: facts and next actions only. The
long reasoning belongs in `docs/history/`.

Last updated: 2026-11-17.

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
| Buttons | ? |
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
| Console log | Ring flusher on SYSTEM (`SANTONILOG1`), pull with `tools/sweet-log-pull.sh` — **works** |
| iOS userland | launchd starts, then dies at code signing (`CS_KILLED`) — **blocked** |
| Touch | DSX at `0x20` probed, F12 programmed, tap poll exists — **not in HID yet** |

## Latest boot (kernel `bb17acf`, patch 0204)

* 0204 fixed a real panic: the early probe's last `printf()` had two `%s`
  and one string argument, so `vprintf_internal` aborted at `far 0x1`
  immediately after the white band.
* With 0204 the boot passes the probe and reaches the **four-bar test** at
  the end of storage init. The user also saw a **green band above the bars**.
* The 0204 ring was pulled: **new log, total 27885 bytes, flush #188**. The
  early probe ran, walked the PDT, found F12, but **rejected F01**
  (`PDT is not real (F01 49 07 83 02, want 00 06 0d 11)`), so
  `dsx_f12_program()` bailed and reporting was never enabled.
* The late F12 poll therefore read **random bytes** every sample; the green
  band was that noise, not a finger. The 300 s watchdog then fired
  (`probe did not finish`) and reset the phone to fastboot.
* The eMMC path stayed healthy through it (CMD1 `OCR 0xc0ff8080`, 8-bit HS,
  GPT, mount, log flusher), except one CMD24 timeout while the poll was
  running.

Image: `shim-boot-touch-early-format.img`,
sha256 `c4c197aa03be58f2bcd568079997396fcc297463c6d46d996d8ff5d77006cdb2`.

## Next actions (in order)

1. **Fix the F01 gate.** The PDT walk returns `4c 00 13 06 01 12` (a perfect
   F12 record) at `0x00dd`, but the entry above it at `0x00e3` is
   `26 0d 04 01 01 81`, which is not `00 06 0d 11`. Either F01 is at another
   address or the walk shifts by one byte above F12. Print both alignments
   and let the F01 record decide; do not program F12 until it matches.
2. Re-run the boot and confirm `f12 ctrl28 <- 0x1f ack` plus a read-back.
3. Then run the tap test and decode X/Y; wire
   `SweetHIDTouch::touch(finger, down, x, y)`.
4. Keep `sweetouchreboot` off while debugging so the phone stays in place.


## Blockers / unknowns

* No finger-down F12 samples yet → the 2D report layout is inferred.
* Boot stop after the four bars is unidentified until the ring is read.
* No local `unixnu build`; every kernel iteration costs a CI run.

## Key files

| Path | Why |
| --- | --- |
| `docs/STATUS.md` | Longer status, artifact table |
| `docs/touch.md` | DSX probe, F12 setup, band map, boot args |
| `docs/history/` | Full engineering log, per month |
| `xnu/patches/0204-whyred-touch-early-format.patch` | Latest patch |
| `tools/build-touch-diag.sh` | Pack a diag image from a CI kernel |
| `tools/sweet-log-pull.sh` | Pull the ring from SYSTEM |

## Device / workflow facts

* Enter fastboot: hold Power ~12 s, wait 5 s, then Power + Volume Down.
* Enter recovery (OrangeFox): Power + Volume Up. adb serial `611026fa`.
* Push target: `ssh://git@ssh.github.com:443/N1709/ios-whyred2.git`,
  branch `whyred`. Never push to `main`.
