# Current status

Last updated: 2026-11-25. The P3 mode experiment (0229-0231) never fixed the
black screen: the 0230 boot stayed sRGB and the user's manual test flashed an
old pre-0229 image. The user confirmed that `35e591a` did reach the lock
screen, and that commit's display recipe is native 1080x2160 canvas,
`/product` sRGB and no mode registry. The series is therefore reverted to
that display state (0229-0231 removed, driver restored to the 0228 tree)
while every current touch/storage patch stays. The recovery image is built
and copied to the laptop; the red unsupported-screen bar is accepted for now.
It is `laptop:/home/n1709/whyred-flash/shim-boot-ring-b971761-native-srgb.img`
(6,176,768 B, sha256
`de50ba30309dd3cf0ba14b7c6256d56db4d61147da9bdbb28c091129ece7991a`,
native canvas with the sRGB product, `sweetlog=1`, `sweetouchpower=1`) and
has not been booted yet.
The last hardware ring remains the already-used 0230 image
`shim-boot-ring-9abe008-p3.img` (5,918,720 B, sha256 `e92bff82…`); do not
boot that same image again.

2026-11-25: the sibling vince TD4310 driver (`unixnu-vince`,
`0099-vince-input.patch`) reads the same part with the register address in
its own START..STOP transaction and a fresh read START; it never uses a
repeated start. Patch `0236` adds that framing (`page|a1stop`) to both the
bit-bang and QUP paths, makes it the default and tries it before the old
matrix. CI run `45613338340` is green.

2026-11-26: **0236 booted and failed.** The ring (`ring-0236.txt`, image
`whyred-ring-0236.img`, sha256 `7002b363…`) shows the same stream: at
`0x00e9` read 1 is F34 `27 00 09 00 21 34`, read 2 is F01
`2f 26 0d 04 01 01`; at `0x00e3` read 1 is `81 4c 00 13 06 01`, read 2 is
`12 0a 29 00 8f d4`. Framing is ruled out. The ring also caught the F01
control read as `0xc7` and the old driver writing `0xc4` back through the
stream pointer. Patch `0237` is therefore a safety gate: it refuses every
F01 control write while `addressable` is false and leaves the addressed path
unchanged. GPIO 66 is not pulsed; it is the Novatek reset (`rules.md` §9),
and the 0197 reset-only boot already left the part returning zeros.
If the address phase still streams, the next patch is the TD4310 TDDI panel
reset (GPIO 53) plus a panel re-init; prepare that sequence before booting.

## One-paragraph summary

The kernel boots from `fastboot boot`, mounts the HFSX iOS root and reaches
backboardd/SpringBoard. The 35e591a-era display (native 1080x2160, sRGB
product, no `ColorElements`) is the last configuration that reached the lock
screen; the later P3 identity made SpringBoard throw on the P3-only
`BoundingPathBitmap` while the runtime mode was sRGB. The recovery series
keeps all touch/storage work and drops 0229-0231, so the display falls back
to the known lock-screen path with the red unsupported-screen bar accepted.
The same boot proved LDO11 can be
power-cycled safely on this unit, but the Synaptics DSX still ignores the register address phase; F12 and
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
* The 35e591a lock-screen image used the native 1080x2160 canvas
  (`sweetfb_native=1`) and an sRGB `/product`; UIKit then draws its red
  unsupported-screen bar, which the user accepts for now.
* `0229`-`0231` (P3 `ColorElements` and the wide-gamut colorimetry) are
  removed from the recovery series. The driver is restored to the 0228 tree,
  so IOMFB publishes no mode arrays and falls back to sRGB exactly like the
  35e591a image.

## Next actions

1. `0237` is pushed (`8578480`). `0238` is written and host-tested: opt-in
   `sweetpanelreset=1` defers the touch probe until userland has presented a
   frame, pulses GPIO 53 (`<1 1>, <0 5>, <1 35>`), replays the TD4310
   on-command, then probes the DSX. Normal boot is unchanged; the workflow
   has a `panel_reset=on` input.
2. Ask the user before the next boot. Prefer the `log` image so a blank
   panel still leaves a ring to pull; the reset runs ~3 s after the first
   userland present, so the lock screen must be up first.
3. If GPIO 53 turns out not to reset the DSX (live DT has no
   `synaptics,reset-gpio`), the result is a clean negative: panel blinks,
   probe still streams. Then 2 us bit-bang (`sweetouchscl=2`) and QUP1
   (SError `0xbf000002`).
4. Once the address phase works, program F01/F12 and verify ordinary tap,
   swipe and multi-touch through `SweetHIDTouch` with no phantom contacts.

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
* Latest green: run `45558050379` (through 0231); the lock-screen recovery
  series reverts 0229-0231, so its next green run starts at 0228.
* Repack a CI kernel with the two-pass `shim/build.sh` recipe in
  `docs/history/2026-11.md`; boot only with `fastboot boot`.

## Latest artifacts

| Image | Kernel | sha256 | Size |
| --- | --- | --- | --- |
| `shim-boot-ring-9abe008-p3.img` (**ready, not booted**) | `9abe008` | `e92bff82405106cf475075c9a7705de6ac6b2eb4893d07bb911beb00864dff22` | 5,918,720 B |
| `shim-boot-ring-e92f698-p3.img` (**already booted**) | `e92f698` | `265e8171d6146353b8b94564815285a585d5264668b2313ee8318b55d57830a8` | 6,176,768 B |
| `images/shim-boot-ring-9dd552a.img` | `9dd552a` | `1cc7a4df27ed62ddadcad8b6f2687fcb1ee644e94acedc20834e82ea7484126f` | 5,918,720 B |
| `images/shim-boot-ring-e92f698.img` | `e92f698` | `c61d267baffcdd2a22a6963df6f50db28ef748a20811b733e53c6149618de00e` | 5,918,720 B |
| `images/shim-boot-ring-e92f698-qup.img` | `e92f698` | `5fd5a252f4daa626517db7f1ca26739cc25272eae3fa3711de5ae30b4ff5e949` | 5,918,720 B |
| `shim-boot-ring-5985a74.img` (last pulled ring) | `5985a74` | — | 5,910,528 B |

The ready image is on the laptop at
`/home/n1709/whyred-flash/shim-boot-ring-9abe008-p3.img`; remote
`sha256sum -c SHA256SUMS.9abe008` passes.

The images built at 16:31 (`474f9a41…`, `b310dcb7…`, `4acef22c…`) and the
native-canvas image (`3bb06e3c…`) are invalid: their ad-hoc cmdline omitted
`emmcrw=1 cs_enforcement_disable=1 unentitled_ios_sim_launch=1`, so launchd
is killed with `CS_KILLED`. The native canvas is now an opt-in flag on
`tools/build-ring-image.sh`.

Older artifact hashes are in `docs/history/` and git history.
