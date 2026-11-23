# Current status

Last updated: 2026-11-23. Latest CI-built kernel: `e92f698` (patches through
`0228`), run `45145911115` green. Local patch `0229` publishes the P3 D65
IOMFB mode and passes the full host suite; it is not built or booted yet.
Run `45210593721` stopped before compile because its host test referenced the
intentionally untracked D22AP ADT; the replacement uses a synthetic fixture.
Dispatch `45212326756` queued on the high-demand M1 label; CI now targets the
standard `macos-15-intel` runner instead.
The latest hardware boot is the already-used
`shim-boot-ring-e92f698-p3.img` (6,176,768 B, sha256 `265e8171…`). Its fresh
ring is `/tmp/opencode/logs/ring-recovery-now.txt` (130,322 B, flush 133).
Do not boot that same image again.

## One-paragraph summary

The kernel boots from `fastboot boot`, mounts the HFSX iOS root and reaches
backboardd/SpringBoard. The complete D22AP `/product` node now correctly
reports P3, but the latest ring proves that the framebuffer runtime mode
still falls back to sRGB. SpringBoard therefore crashes while loading the P3
`BoundingPathBitmap-1125x2436-375x812-3.00x`, producing the current black
screen. Patch 0229 supplies the missing P3 `ColorElements`/`TimingElements`
and active IDs. The same boot proves LDO11 can be power-cycled safely on this
unit, but the Synaptics DSX still ignores the register address phase; F12 and
the HID poll remain safely disabled. Touch must become normal only after the
lock screen is visible.

## Where the boot stands (2026-11-23)

* The latest ring proves `0224`: LDO11 is APPS-owned, disable/enable ACK and
  readback remains `0xa0`; the fresh address test still streams.
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
* `0228` is present in the latest boot: the ring has four presents before
  display-off and none afterwards. The SpringBoard crash prevents a clean
  visual power-button verdict.
* The default canvas is the iPhone X 1125x2436, cropped to the 1080x2160
  panel (`fOffX=20`, `fOffY=138`), which keeps UIKit's red unsupported-screen
  clock away. `SWEETFB_NATIVE=1` is the opt-in native canvas.
* The latest mode is the correct logical 1125x2436 at 60 Hz but still says
  `sRGB SDR`; the real product node says P3. That mismatch, not the canvas,
  is the current SpringBoard crash.
* `0229` uses the confirmed IOAV values `Colorimetry=14` (DCI-P3 D65),
  `EOTF=0` (SDR gamma), RGB 8-bit/full range, with color/timing ID 1 and
  selector 79 block `0x40` returning those IDs at `+4`.

## Next actions

1. Commit/push 0229 and the complete `/product` fix; require a green WHYRED
   CI build with no skipped patch.
2. Pack a new ring image (same safe iOS-root flags, P3 ADT, no
   `sweetfb_native=1`) and report exact path, size and sha256.
3. Boot only that new image with `fastboot boot`; require the ring to show
   both the driver's `P3 D65 SDR` line and `FBSDisplayMode ... P3 SDR`, then
   confirm SpringBoard no longer throws the BoundingPath exception.
4. Once the lock screen is visible, resume DSX work: test a separate
   general-call/QUP diagnostic, solve the address phase, program F01/F12,
   and verify ordinary tap, swipe and multi-touch through `SweetHIDTouch`
   with no phantom contacts.

## Touch: what is known

* The part is the **Synaptics DSX/RMI4 at `0x20`**, IRQ GPIO 67, rail
  `vcc_i2c` = PM660 LDO11. GPIO 66 is the Novatek reset, not the DSX's.
* The early probe (before eMMC) is the only reliable window. It streams the
  PDT: F34, F01 `2f 26 0d 04 01 01`, F12 `4c 00 13 06 01 12`; F01 command
  base `0x26`, F12 data base `0x06`.
* The part ignores the address phase after the first read and keeps a read
  pointer (LPWG/streaming state). 0195/0222/0225 gate every F12 write on
  `dsx_addressable`; a still-streaming part is left alone.
* The latest boot proves an APPS-owned LDO11 disable/enable cycle succeeds,
  but it does not restore addressing. Next diagnostics keep general-call
  reset and QUP framing separate; recovery remains read-only.
* F12 query block: `01 01 01 02 00 00 00 0e 0f 15 c7 11 12 33 13 49 07 83
  02 0b 81 04 03`.
* Missing: a coherent addressed read or a successful wake from LPWG, so the
  2D report layout is still inferred.

## Build and CI

* There is no local XNU build; each kernel change is one CI run
  (~10-15 min). Run `bash tests/run_tests.sh` before committing.
* Latest green: `e92f698`, run `45145911115`; 0229 is local/host-green only.
* Repack a CI kernel with the two-pass `shim/build.sh` recipe in
  `docs/history/2026-11.md`; boot only with `fastboot boot`.

## Latest artifacts

| Image | Kernel | sha256 | Size |
| --- | --- | --- | --- |
| `shim-boot-ring-e92f698-p3.img` (**already booted**) | `e92f698` | `265e8171d6146353b8b94564815285a585d5264668b2313ee8318b55d57830a8` | 6,176,768 B |
| `images/shim-boot-ring-9dd552a.img` | `9dd552a` | `1cc7a4df27ed62ddadcad8b6f2687fcb1ee644e94acedc20834e82ea7484126f` | 5,918,720 B |
| `images/shim-boot-ring-e92f698.img` | `e92f698` | `c61d267baffcdd2a22a6963df6f50db28ef748a20811b733e53c6149618de00e` | 5,918,720 B |
| `images/shim-boot-ring-e92f698-qup.img` | `e92f698` | `5fd5a252f4daa626517db7f1ca26739cc25272eae3fa3711de5ae30b4ff5e949` | 5,918,720 B |
| `shim-boot-ring-5985a74.img` (last pulled ring) | `5985a74` | — | 5,910,528 B |

The images built at 16:31 (`474f9a41…`, `b310dcb7…`, `4acef22c…`) and the
native-canvas image (`3bb06e3c…`) are invalid: their ad-hoc cmdline omitted
`emmcrw=1 cs_enforcement_disable=1 unentitled_ios_sim_launch=1`, so launchd
is killed with `CS_KILLED`. The native canvas is now an opt-in flag on
`tools/build-ring-image.sh`.

Older artifact hashes are in `docs/history/` and git history.
