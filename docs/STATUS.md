# Current status

Last updated: 2026-11-23. Latest built kernel: `9dd552a` (patches through
`0226`), run `45037234274` green. Patch `0227` (QUP writes, chunked HID poll)
is committed and queued for its first CI build. Diagnostic image:
`images/shim-boot-ring-9dd552a.img`, 5,918,720 B, sha256
`474f9a419dc766f062e94e1585c51072302653dec497789e881b437e866219a9`.
The last pulled ring predates this image (`5985a74`), so 0224-0227 are still
unproven on the phone.

## One-paragraph summary

The kernel boots from `fastboot boot` into our XNU, brings up eMMC at 8-bit
high-speed, parses the GPT, mounts the HFSX iOS root read-write, starts the
console log flusher on SYSTEM and drives the panel. On the October v12 root,
launchd reaches backboardd and SpringBoard and the lock screen. The touch
controller (Synaptics DSX at I2C `0x20`) is probed before eMMC and streams
its PDT, but the address phase is still ignored, so the HID poll stays off.
0224-0226 add a rail re-probe, panel-linked DSX sleep/wake and a bounded
stayon veto; 0227 adds QUP writes and a FIFO-sized HID poll. All are
committed and built or queued, not yet booted. The 2026-11-22 black-screen
report came from the older `5985a74` image: XNU and SpringBoard were alive
(1,060 presents, no panic), so it was a userland boot stage, not a 0226
regression.

## Where the boot stands (2026-11-23)

* `0224` gives `whyred_touch_power_cycle()` three results (absent, on after
  an APPS-owned toggle, refused); only a real toggle re-runs the address
  test before F12 is programmed.
* `0225` pauses the HID poll and writes F01 `SENSOR_SLEEP` on display off;
  display on writes `NORMAL_OPERATION | NO_SLEEP_ON`, re-programs F12 and
  only then resumes. `sweetouchgcall=1` is the opt-in general-call reset.
* `0226` bounds `sweetfb_stayon` to the window before the first userspace
  present, calls the weak `whyred_touch_display_power()` on the change, and
  fades the scanned framebuffer to black on whyred (the LAVENDER config has
  no DSI mapping).
* `0227` sends the DSX page select and register+data through
  `sweet_i2c_xfer()` when QUP1 owns the bus, so F12 can be programmed after
  a successful QUP read, and splits the ten-finger HID poll into three-record
  chunks to stay inside the FIFO's 32-byte minimum.
* The default canvas is the iPhone X 1125x2436, cropped to the 1080x2160
  panel (`fOffX=20`, `fOffY=138`), which keeps UIKit's red unsupported-screen
  clock away. `SWEETFB_NATIVE=1` is the opt-in native canvas.
* The last ring (`5985a74`) has the old `display off ignored
  (sweetfb_stayon)` line and none of the 0225/0226 strings. Ring identity is
  now checked with `tools/ring-verdict.sh` before drawing conclusions.

## Next actions

1. Boot `images/shim-boot-ring-9dd552a.img`; run
   `tools/ring-verdict.sh` on the pulled ring. It must report post-0225,
   not the old `5985a74` image.
2. Read `address phase still ignored, not programming` and confirm there are
   no phantom `SWEET HID: touch` lines.
3. If the gate holds, boot a diag with `sweetouchpower=1`; toggle only an
   APPS-owned LDO11 APID, otherwise implement the RPM regulator request.
   Try `sweetouchgcall=1` as a separate test. Once 0227 is built, boot a
   separate `sweeti2c=1` diag and read `DSX reads over QUP1`, the address
   test and the first QUP-backed F12 write.
4. If the address phase works, confirm `f01 ctrl -> 0x84` (NO_SLEEP_ON),
   `f12 ctrl20 <- ... 00`, `f12 ctrl28 <- 0x1f ack`, then decode X/Y.
5. Press power after the first frame; expect `f01 ctrl ... (sleep)`,
   `display off, HID paused, DSX asleep`, then the wake sequence.
6. Keep `SWEETREBOOT=0` while debugging; investigate the CMD24
   `WRITE_BLOCK` timeout separately.

## Touch: what is known

* The part is the **Synaptics DSX/RMI4 at `0x20`**, IRQ GPIO 67, rail
  `vcc_i2c` = PM660 LDO11. GPIO 66 is the Novatek reset, not the DSX's.
* The early probe (before eMMC) is the only reliable window. It streams the
  PDT: F34, F01 `2f 26 0d 04 01 01`, F12 `4c 00 13 06 01 12`; F01 command
  base `0x26`, F12 data base `0x06`.
* The part ignores the address phase after the first read and keeps a read
  pointer (LPWG/streaming state). 0195/0222/0225 gate every F12 write on
  `dsx_addressable`; a still-streaming part is left alone.
* F12 query block: `01 01 01 02 00 00 00 0e 0f 15 c7 11 12 33 13 49 07 83
  02 0b 81 04 03`.
* Missing: a coherent addressed read or a successful wake from LPWG, so the
  2D report layout is still inferred.

## Build and CI

* There is no local XNU build; each kernel change is one CI run
  (~10-15 min). Run `bash tests/run_tests.sh` before committing.
* Latest green: `9dd552a`, run `45037234274`.
* Repack from a CI kernel with `tools/build-touch-diag.sh` or the recorded
  `build-whyred-9dd552a.sh`; boot only with `fastboot boot`.

## Latest artifacts

| Image | Kernel | sha256 | Size |
| --- | --- | --- | --- |
| `images/shim-boot-ring-9dd552a.img` | `9dd552a` | `474f9a419dc766f062e94e1585c51072302653dec497789e881b437e866219a9` | 5,918,720 B |
| `images/shim-boot-ring-9dd552a-native.img` | `9dd552a` | `3bb06e3c9eaf10c15f556b9654e9c3172b5ce58f9a4990828078f6d7d43156b5` | 5,918,720 B |
| `shim-boot-ring-5985a74.img` (last pulled ring) | `5985a74` | — | 5,910,528 B |

Older artifact hashes are in `docs/history/` and git history.
