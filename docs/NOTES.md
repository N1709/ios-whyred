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
  GIC SPI 95), SDA GPIO 2, SCL GPIO 3, reset 66, IRQ 67, rail `pm660_l11`.

## History index

| File | Range |
| --- | --- |
| [`history/2026-10.md`](history/2026-10.md) | October 2026: shim, CPU, memory, SDHCI/eMMC, root mount, code signing, first touch probes |
| [`history/2026-11.md`](history/2026-11.md) | November 2026: QUP vs bit-bang, PDT/page-select, F12 reporting, panel bands, 0204 format fix |

The archive is append-only. New entries go to the month file and a one-line
summary goes into `progres.md`.
