# ios-whyred — working notes

This file used to be the full engineering log (5,700+ lines). It is now the
short index; the log lives in `history/`, one file per month, newest month last.

## Start here

| File | What |
| --- | --- |
| [`progres.md`](progres.md) | Live board: what works, latest boot, next actions |
| [`AGENTS.md`](AGENTS.md) | Environment, workflow and reference sources |
| [`rules.md`](rules.md) | Hard rules for AI agents (safety, git, CI, patch discipline) |
| [`STATUS.md`](STATUS.md) | Longer status and the latest image hashes |
| [`touch.md`](touch.md) | Synaptics DSX probe, F12 setup, panel band map |
| [`patches.md`](patches.md) | The XNU patch series |
| [`history/2026-11.md`](history/2026-11.md) | Newest month of the log |
| [`README.md`](../README.md) | Overview, what works, how to boot |

## Project in one paragraph

Boot Apple's iOS 16.7.16 (iPhone X userland) on the Xiaomi Redmi Note 5
(`whyred`, SDM636) with a port of XNU 8796. The kernel is started from RAM
with `fastboot boot`; nothing is flashed. Display and storage use the kernel's
own drivers. eMMC, the HFSX root, the framebuffer and the console log all work
on hardware. With `emmcrw=1 cs_enforcement_disable=1
unentitled_ios_sim_launch=1`, launchd reaches backboardd and SpringBoard;
without them, initproc dies with `CS_KILLED` (wrong platform). The touch
controller (Synaptics DSX at I2C `0x20`) is probed and its F12 reporting is
being brought up, but no touch data reaches HID yet.

## Hardware facts

* SoC: 4× Cortex-A73 Gold (`MPIDR 0x100`–`0x103`, 16K pages) + 4× A53 Silver
  (parked). DRAM base `0x80000000`, safe budget `0xa0000000` + 1 GiB.
* GICv3 at `0x17a00000`, timer 19.2 MHz.
* eMMC: SDCC v5 at `0x0c0c4000` (GIC SPI 110), 8-bit high-speed,
  122,142,720 sectors, HFSX root in `userdata` mounted read-write.
* Panel: 1080×2160, continuous splash at `0x9d400000`, 32-bit `a8r8g8b8`,
  stride 4320.
* Touch: Synaptics DSX/RMI4 at `0x20` on BLSP1 QUP1 (`0x0c175000`,
  GIC SPI 95), SDA GPIO 2, SCL GPIO 3, IRQ 67, rail `pm660_l11`. The DT
  has no DSX reset GPIO; GPIO 66 is the **Novatek** NT36672 reset.

## Driver handover (2026-11-25, for Claude)

* Tip `4907c72` (docs); touch code `d0d3ebc`/`86d469d`. CI `45613338340`
  (tip `86d469d`) is **green**. Host suite green.
* **0236 booted and failed.** Ring pulled from SYSTEM
  (`ring-0236.txt`; local copy `/tmp/opencode/ring-0236.txt`). The vince
  frame `page|a1stop` was used for every read and the address phase is still
  ignored: at `0x00e9` the first read returns F34 `27 00 09 00 21 34`, the
  second returns F01 `2f 26 0d 04 01 01`; at `0x00e3` it returns
  `81 4c 00 13 06 01` then `12 0a 29 00 8f d4`. The pointer streams the
  register map in walk order. **Framing is not the cause; do not add another
  framing.**
* Image already booted, do not boot it again: `whyred-ring-0236.img`,
  sha256 `7002b363d2f35dc274045738747f38316673f1fdc2ed974bfc0200d088d15116`.
* New root-cause candidate: the panel is a **Tianma TD4310 TDDI**
  (`dsi_td4310_tianma_fhd_video`); display and touch are one chip. The stock
  DT comments out `synaptics,reset-gpio` because the **MDSS panel reset**
  owns it: `qcom,platform-reset-gpio = <&tlmm 53 0>` in
  `sdm660-mtp.dtsi`, TE `qcom,platform-te-gpio = <&tlmm 59 0>`. GPIO 66 is
  the **Novatek** reset; the `sweetouchreset=1` ring proved it does nothing
  to this DSX.
* Recovery evidence (OrangeFox, stock kernel): `1-0020` binds, the PDT scan
  works, `productinfo 0x01 0x00`, `buildid 2788281`, `flashprog 0`,
  `fwu_go_nogo`: device fw `2788281` vs image `2732044`, no reflash. The
  chip is in app mode after the stock display/touch init, and the iOS kernel
  never performs that init.
* Next candidates, in order:
  1. Pulse the TD4310 reset (GPIO 53) once, then re-init the panel. The
     display may blank; `SweetFramebuffer` can send DCS over DSI0 but has no
     TD4310 init sequence, so plan the panel re-init before this boot.
  2. Speed up the bit-bang: `TOUCH_I2C_DELAY_US` is 10 (about 33 kHz); the
     stock QUP runs at 400 kHz. Try 2 us, same framing.
  3. Fix the QUP1 bring-up. The `--qup` image panics at the first
     `qup[QUP_HW_VERSION]` read ("QUP mapped, probing the SE"): the BLSP1
     QUP1 register window is dead (CX/NoC or clock). Bit-bang is the only
     safe path until this is fixed.
* Read `docs/touch.md` and `docs/STATUS.md` first. The brain note in
  `brain_ai` (`brain/ios-whyred/2026/11-November/25-Wednesday/`) has the
  long timeline but is not pushed yet.

## History index

| File | Range |
| --- | --- |
| [`history/2026-10.md`](history/2026-10.md) | October 2026: shim, CPU, memory, SDHCI/eMMC, root mount, code signing, first touch probes |
| [`history/2026-11.md`](history/2026-11.md) | November 2026: QUP vs bit-bang, PDT/page-select, F12 reporting, panel bands, 0204 format fix |

The archive is append-only. New entries go to the month file and a one-line
summary goes into `progres.md`.
