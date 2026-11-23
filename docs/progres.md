# progres.md — live progress board

Update this file at every milestone, before handing off, and whenever a boot
result changes the picture. Keep it short: facts and next actions only. The
long reasoning belongs in `history/`.

Last updated: 2026-11-23.

## What works

| Feature | State |
| --- | --- |
| Boot (`fastboot boot`) | **Works** |
| CPU / memory | **Works** |
| Display / framebuffer | **Works** |
| Storage (eMMC + HFSX root) | **Works** |
| Console log ring | **Works** |
| Kernel log / panic dump | **Works** |
| Touch (probe) | **Works** |
| Touch (as HID input) | Latest P3 ring: LDO11 power-cycle succeeds, but the DSX still ignores the address phase; F12 and HID remain safely off. 0227's QUP path is still untested |
| Power button / display off | Latest ring reaches `display off, HID paused`; 0228 prevents later presents. Visual blanking is confounded by the SpringBoard crash |
| Buttons | Added (0208), not booted |
| RTC / date | Added (0209), not booted |
| eMMC ADMA2 reads | Added (0210), not booted |
| Panel 60 Hz / present timing | Added (0207), not booted |
| GPU / Metal | Ladder + IOKit service added (0211/0217), not booted |
| Wi-Fi | Added (0214), not booted |
| Bluetooth | No |
| Audio | Added (0213), not booted |
| Cellular | Added (0215), not booted |
| Wallpaper / PosterBoard | Lever added (0216), not booted |
| Charging | Static 100% fallback; no PM660 gauge |
| iOS userland (launchd → SpringBoard) | Historical root reached the lock screen; latest P3 boot reaches SpringBoard but crashes on `BoundingPathBitmap` because IOMFB still reports sRGB |

Legend: **Works** = verified on the phone; **Added** = code exists but has
not booted; **No** = not implemented or known blocked; **Static** = a fixed
fallback value, not a measurement.

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
| Display | Kernel framebuffer works; latest ring presents the correct 1125x2436 iPhone X canvas. Runtime gamut is still sRGB; local patch 0229 publishes P3 D65, not built yet |
| Console log | Ring flusher on SYSTEM (`SANTONILOG1`), pull with `../tools/sweet-log-pull.sh` — **works** |
| iOS userland | launchd → backboardd → SpringBoard; latest boot crashes before the visible lock screen because the P3 asset facet is paired with an sRGB `FBSDisplayMode` |
| Touch | DSX at `0x20` streams the PDT; the address phase is still ignored even after the latest ring's successful LDO11 power-cycle. No phantom HID poll starts |

## Latest boot: P3 product is correct, IOMFB is still sRGB (2026-11-23)

The already-booted `shim-boot-ring-e92f698-p3.img` is 6,176,768 B, sha256
`265e8171d6146353b8b94564815285a585d5264668b2313ee8318b55d57830a8`.
It has the real 63-property D22AP `/product` node with
`artwork-display-gamut=P3`, the iPhone X 1125x2436 canvas, no
`sweetfb_native=1`, and all three iOS-root exec flags. Do not boot this same
image again.

The fresh recovery pull is `/tmp/opencode/logs/ring-recovery-now.txt`
(130,322 B, flush 133). It proves:

* backboardd still reports `375x812@3x ... 60Hz sRGB SDR`;
* SpringBoard crashes on
  `BoundingPathBitmap-1125x2436-375x812-3.00x` because its asset facet is P3;
* LDO11 toggles successfully, but the DSX remains streaming and F12/HID are
  correctly skipped;
* four frames are presented, then `display off, HID paused`; there are no
  later presents in this ring.

Local patch `0229-whyred-fb-p3-mode.patch` now publishes
`TimingElements`/`ColorElements` as RGB 8-bit, P3 D65 (`Colorimetry=14`),
SDR (`EOTF=0`), and returns color/timing ID 1 from selector 79 block `0x40`.
The full host suite passes. It is **not built or booted** yet.

Progress estimates: black screen/SpringBoard 78%, touch 70%, status bar 70%,
GPU 35%, overall 61%.

## Current next actions

1. Commit and push 0229 plus the complete-P3 product-node fix; wait for a
   green WHYRED CI kernel.
2. Pack a **new** ring image with the existing e92f698 command line, P3 ADT,
   no `sweetfb_native=1`; report its path, size and sha256.
3. Boot it once with `fastboot boot`, pull a fresh ring, and require both
   `SWEET FB: ... P3 D65 SDR` and `FBSDisplayMode ... P3 SDR` before calling
   the black-screen fix successful.
4. After the lock screen is visible, continue the DSX address-phase work
   until normal tap/swipe/multi-touch reaches `SweetHIDTouch`; test the QUP
   image separately. Then continue the real Adreno 509 path.

## CS_KILLED from a bad image cmdline, corrected (2026-11-23)

The first boot of the 2026-11-23 diagnostic series died at initproc with
`SWEET EARLY PANIC: unexpected SIGKILL of launchd (CS_KILLED)` (the report
said “launcher”; the kernel prints `p_name`). XNU only panics there for
initproc (`bsd/kern/kern_sig.c`), and `CS_KILLED` means the kernel killed it
for code signing.

Cause: the three images had been packed by an ad-hoc script whose cmdline
omitted `emmcrw=1 cs_enforcement_disable=1 unentitled_ios_sim_launch=1`. The
last working ring (`5985a74`) carries all three, and
`docs/history/2026-10.md` records the same panic as `OS_REASON_EXEC` /
`EXEC_EXIT_REASON_WRONG_PLATFORM` with `unentitled_ios_sim_launch=1` as the
fix. The kernel binary is unchanged; only the payload cmdline was wrong.
This is a build-recipe regression, not a 0227/0228 regression.

Corrected images (same kernels, rebuilt payload), all 5,918,720 B:

| Image | sha256 |
| --- | --- |
| `images/shim-boot-ring-9dd552a.img` | `1cc7a4df27ed62ddadcad8b6f2687fcb1ee644e94acedc20834e82ea7484126f` |
| `images/shim-boot-ring-e92f698.img` | `c61d267baffcdd2a22a6963df6f50db28ef748a20811b733e53c6149618de00e` |
| `images/shim-boot-ring-e92f698-qup.img` | `5fd5a252f4daa626517db7f1ca26739cc25272eae3fa3711de5ae30b4ff5e949` |

The old hashes `474f9a41…`, `b310dcb7…` and `4acef22c…` are invalid and must
not be booted. `tools/build-ring-image.sh` now encodes the recipe, and
`tests/run_tests.sh` checks the three flags on its assignment line.

## Last boot: pre-0225 image, XNU alive (2026-11-23)

The ring pulled on 2026-11-22 22:42 belongs to
`shim-boot-ring-5985a74.img`: its boot-args carry `sweetfb_native=1` and the
old `display off ignored (sweetfb_stayon)` line, and none of the 0225/0226
strings. XNU was alive (SpringBoard and backboardd exec, 1,060 presents, no
panic), so the reported black screen with a progress bar was not a kernel
hang and not the new display-power path. `nxprov` is not in that ring; it is
Skywalk's normal provider registration from a console-image boot.

Next: boot the corrected `images/shim-boot-ring-9dd552a.img` (5,918,720 B,
sha256 `1cc7a4df…`) and run `tools/ring-verdict.sh` on the fresh ring before
judging the touch or display changes.

## Panic 0220/0221 (2026-11-22)

The 0220 image stopped at `SWEET EARLY PANIC: Unhandled implementation
specific error`. That text is XNU's SError path (`osfmk/arm64/sleh.c`), not
a normal data abort: some MMIO access raised an asynchronous bus error. The
new default cmdline forced `sweetouchpower=1` and `sweeti2c=1`; LDO11 is an
RPM-managed regulator on whyred (`qcom,rpm-smd-regulator`), so the APPS
does not own its SPMI APID. The direct rail toggle is the prime suspect.

The empty ring was the second clue: 0220 called the toggle from
`whyred_touch_early()`, which runs before `santoni_log_start()`, so an SError
there panics before a single line reaches SYSTEM. The ownership check is not
enough, because the fault can happen on the first arbiter read.

0220 therefore keeps the toggle **late-only**, leaves it off unless
`sweetouchpower=1`, and `tools/build-touch-diag.sh` now needs
`SWEETOUCHPOWER=1` to add it. 0221 prints `pc/lr/sp/far/esr` on this panic
so the next failure names its PC. The normal image keeps the proven bit-bang
probe (`sweetouchi2c=1 sweetouchwait=0`). The F01 sleep clear and the F12
`ctrl20` wakeup-gesture clear stay, so the DSX can still leave LPWG.

Next boot: confirm the panic is gone, then read `WHYRED TOUCH: address
phase`, `f01 ctrl ... -> 0x80`, `f12 ctrl20 <- ... 00`, `f12 ctrl28 <-
0x1f ack`, and a finger tap in the HID poll.

## Watchdog and F12 gate 0222 (2026-11-22)

The `253d75e` normal image gave no usable log: the normal image has no
SYSTEM ring, and `whyred_touch_early()` still armed a 300 s watchdog for
every `sweetouchearly` boot. The late probe also wrote `ctrl20`/`ctrl28`
through F12 bases parsed from the stream while `dsx_addressable == 0`, then
started the HID thread on the same untrusted base.

`0222` changes both:

* The reboot watchdog is armed only when `sweetouchreboot=1` (the diag image
  that intentionally returns to fastboot) and is cancelled at the end of the
  probe. A normal or log boot cannot be reset by it.
* `dsx_f12_program()`, the 12-sample `dsx_f12_poll()`, `dsx_touch_test()` and
  `dsx_hid_start()` all require `dsx_addressable`. A still-streaming part is
  left alone; the ring says `address phase still ignored, ...`.

The next boot is the **log image** (`build_mode=log`): the ring records the
touch lines and the userland path even if the panel stays on the Apple logo.
Pull it from recovery with `tools/sweet-log-pull.sh`.

## Fast probe and quick images 0223 (2026-11-22)

The old probe spent ~87 s between `early probe requested` and `early done`:
the open-drain wait was 2 ms per stretched bit and the probe read the stream
map twice. `0223` lowers the default wait to 500 us, adds
`sweetouchscl=<us>`, and adds `sweetouchfast=1`, which skips the two `0x200`
reads and uses the F01/F12 bases this unit already streamed. The address
test still runs, and 0222 keeps every F12 write gated on it.

The workflow has a new `build_mode=quick` that produces both requested
variants from one kernel:

* `shim-boot-fast.img` — quiet normal image plus `sweetouchscl=200
  sweetouchfast=1` (B: boot fast, touch stays).
* `shim-boot-ring.img` — 4 MiB SYSTEM ring (`sweetlog=1`) with no
  `cs_debug=2` page dumps and no kprintf spam, plus the same short probe
  (C: boot fast, still get a readable log).
* `shim-boot-console.img` — opt-in `build_mode=console`: the panel draws
  the running console text over the GUI (`disable_kprintf_output=0`, no
  `sweetfb_noconsole`), while the 4 MiB ring and the short probe stay. Use
  it when the panel itself has to show where the boot stops.

The console image is the one to boot next when the screen must show the
running text; the ring image is the quieter equivalent.

## Rail re-probe, display link, power button and QUP writes 0224-0228 (2026-11-22, not booted)

`0224` changes `whyred_touch_power_cycle()` to return three states: `1` for a
rail that is on after the toggle, `0` when `sweetouchpower` is absent, `-1`
when the arbiter refuses the APID. The late probe re-tests the address phase
only on `> 0`, before `dsx_f12_program()`, so a refused toggle cannot be
mistaken for a fresh window.

`0225` gives panel and digitizer one power story. When the late probe has
finished, a display-off request pauses the HID poll, waits for an in-flight
read, and writes F01 `SENSOR_SLEEP` (bit 0). Display-on writes
`NORMAL_OPERATION | NO_SLEEP_ON` (bit 2, preserved `CONFIGURED` bit 7),
programs F12 again, and resumes the poll. This is the vendor's
`synaptics_rmi4_sleep_enable()` sequence, not a new guess. A part that never
reached the address phase has nothing to suspend. The same patch adds an
opt-in I2C general-call reset (`sweetouchgcall=1`) for the case where the
part ignores the address phase entirely.

`0226` makes `sweetfb_stayon=1` a **boot hold**, not a permanent veto: it
ignores display-off only before the first userspace present. After that the
power button and auto-lock can blank the panel; the framebuffer calls
`whyred_touch_display_power()` on the transition. whyred has no DSI/MDP
mapping, so there is no DCS panel-off: the vsync timer fades the boot
framebuffer to black in place and idles.

`0227` closes the QUP write gap. Once `sweeti2c=1` made QUP1 the bus master,
`dsx_write_f()` still returned `EIO`, so F12 could never be programmed on
that path even if a read addressed the part. The QUP branch sends the page
select and the register plus data through `sweet_i2c_xfer()`, which already
supported write-only transfers. The ten-finger HID read is split into
three-record chunks on QUP to stay inside the FIFO's documented 32-byte
minimum; the bit-bang read stays whole.

`0228` closes the other half of display-off. The 2026-11-22 ring has 1,057
presents after its first display-off request and 744 after the second, so
backboardd does not stop drawing when the request is ignored. On whyred the
panel is blanked only by the software fade into the boot framebuffer; a
present would overwrite it. `present()` now returns while `fPowerOff` is set,
and display-on's existing `fDirty = true` makes the vsync timer repaint the
latest pending swap.

Build state: CI run `45145911115` (`e92f698`) is green with 0227/0228. Two
ring images were packed from that kernel and later rebuilt with the three
iOS-root flags: `images/shim-boot-ring-e92f698.img` (sha256 `c61d267b…`, no
QUP) and `images/shim-boot-ring-e92f698-qup.img` (sha256 `5fd5a252…`,
`sweeti2c=1`). The corrected `images/shim-boot-ring-9dd552a.img` (sha256
`1cc7a4df…`, `sweetouchpower=1`) stays the first boot because it isolates
0224-0226 from the QUP and present-guard changes. The first 2026-11-23
images died at initproc with `CS_KILLED`; see the top section. No image
after `5985a74` has completed a boot on the phone.

`shim/build.sh` no longer puts `sweetfb_native=1` on the default command
line. The default ADT canvas is again 1125x2436, and `SweetFramebuffer`
crops it to 1080x2160, which removes the red unsupported-screen clock.
`SWEETFB_NATIVE=1` is now the opt-in path that publishes 1080x2160 and
brings the red fallback back. CI rejects `sweetfb_native=1` in every image
it builds.

Next boot: the quick ring image first. Confirm no phantom `SWEET HID: touch`
lines, then read `address phase`, `f01 ctrl`, `f12 ctrl20/ctrl28`. If the
address phase still fails, build a diag with `sweetouchpower=1` and read the
LDO11 owner; optionally add `sweetouchgcall=1` to test the general call. If
it works, check `f01 ctrl ... 0x84` (NO_SLEEP_ON), tap the panel, then press
power and look for `display off, HID paused, DSX asleep`.

## New fixes 0207-0210 (2026-11-22, not booted)

Four small SDM660 fixes from `pcorezzola/lavender-ios16` are now whyred
patches. They are local code only: no CI build and no boot yet.

| Patch | What it adds | Boot-arg |
| --- | --- | --- |
| 0207 | Display default 60 Hz, vsync gated on flush ticks, per-swap telemetry and `present()` timing | `sweetfb_hz`, `sweetfblog` |
| 0208 | PM660/PM660L keys over SPMI arbiter v3, started from `santoni_sdhci.c` | `sweetkeys=0` disables |
| 0209 | PM660 RTC through the same arbiter; skips the 30 s IORTC wait; calendar fallback | `sweetrtc`, `sweetrtcoff` |
| 0210 | SDCC v5 exact PIO/cache state, post-write DAT0/CMD13 wait, DMA diagnostics, ADMA2 reads with PIO fallback | `sweetemmc_dma=0`, `sweetemmc_speed=1`, `sweetemmc_dmatest=1` |

All 234 patches apply on the whyred series tip; `drivers/` mirrors are
regenerated. The next boot must confirm the banner `driver 0210`, the
ADMA2 self-test, `SWEET KEYS:` lines, `SWEET RTC:` raw/offset and the
`SWEET FB: present ... us` budget.

## Big drivers 0211-0215 (2026-11-22, not booted)

The four large SDM660 drivers from `pcorezzola/lavender-ios16` are now
whyred sources, renamed `whyred_*` and gated by their own boot args (all
off by default). Lavender's fixed modem LBA is gone: both loaders ask the
GPT for the `modem` entry.

| Patch | What it adds | Boot-arg |
| --- | --- | --- |
| 0211 | Adreno 512 ladder: GCC/GPUCC, BIMC votes, CX/GX GDSC, GFX3D root, resets, GPU zap query | `whyredgpu=N`, `whyredgpudelay=S` |
| 0212 | Read-only `santoni_emmc_read()` and the GPT modem LBA | none |
| 0213 | LPASS/ADSP audio: maps, GDSC/SMMU order, PAS auth, firmware from the modem FAT | `whyredaud=N`, `whyredauddelay=S`, `whyredaudforce=1` |
| 0214 | WCN3990 platform probe through the ANOC2 SMMU | `whyredwlan=1`, `whyredwlandelay=S` |
| 0215 | MSS/QDSP6 modem: clocks, RMB, reset, MBA/MDT segments, PAS auth | `whyredmodem=N`, `whyredmodemdelay=S`, `whyredmodemclk=MASK` |

None of this has run on the phone. The first boot must show the SDHCI
`"modem" first block` line before the loaders touch eMMC; without it they
stop and log `no "modem" GPT partition`.

## Wallpaper 0216 (2026-11-22, not booted)

iOS 16's wallpaper path fails in three places on this port: PosterBoard
traps on the lock-screen prewarm, the gallery has no
`com.apple.posterkit.provider` extension point, and the home-screen preview
builds a Metal command queue that does not exist. `0216` is the first
lever only: `sweetnopb=1` makes the exec of
`/Applications/PosterBoard.app/PosterBoard` fail with `EPERM` and prints
`SWEET EXEC: refusing PosterBoard`. If the lock screen stays alive without
the poster process, the freeze follows PosterBoard; if it still does not
update, the poster is not the gate. Default off. The CPU preview fallback
and the extension-point work are userland changes, not in the kernel patch.

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

## The 2026-11-22 console ring: no hang, but a skipped touch chain

The console image (`becdfbe`) was booted and the panel stopped at
`Skywalk: sk_zero_128 ... PASSED`. The SYSTEM ring from that boot proves the
kernel did **not** hang: it contains `exec launchd`, `fsck`,
`exec SpringBoard`, `exec backboardd`, `exec watchdogd`, FIPSPOST
`all tests PASSED`, and key/touch polling up to the reset. The panel text
stops because the framebuffer console stops drawing after the userspace
handoff; the ring keeps logging.

The same boot exposed a build bug. CI skipped `0219` because its
`bsd/conf/files` hunk still expected the old `sweet_i2c1.c` neighbour (the
tree now has `whyred_power.c` there). `0220`, `0222` and `0223` then failed
because `whyred_touch.c` did not exist. dxb only warns on a skip, so the
"latest" image still ran the old touch code: it wrote F12 while
`dsx_addressable == 0` and polled random bytes, which SweetHID reported as
ten fingers at the screen edges. The `f12 ctrl28 <- 0x1f ack` and the edge
touches in the ring are that old code, not the 0222 gate.

`0219` now has a minimal three-line `bsd/conf/files` hunk, a host test keeps
it free of the stale neighbour, and CI fails if any of `0219..0223` is
skipped. Rebuild pending.

## Next actions (0219-era, superseded)

The 2026-11-23 corrected images and the current ordered boot plan are at the
top of this file and in `STATUS.md`; this older list is kept only for the
0219 context above. Do not boot the pre-correction hashes. The current plan
still proves the same points: ring identity, no phantom HID lines, power
fade/sleep/wake, `presents after display off: 0`, then the QUP write path,
and SDHCI CMD24 separately.


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
| `../xnu/patches/0228-whyred-fb-drop-off-presents.patch` | Latest patch (0228) |
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
