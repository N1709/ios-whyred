# Current status

Last updated: 2026-11-17, kernel `87b7805` (patch 0205), image
`shim-boot-touch-f01-scan.img`.

## One-paragraph summary

The kernel boots from `fastboot boot` into our XNU, brings up eMMC at 8-bit
high-speed, parses the GPT, mounts the HFSX iOS root read-write, starts the
console log flusher on SYSTEM and paints the four-bar framebuffer test.
The **touch controller** (Synaptics DSX at I2C `0x20`) is probed before eMMC,
its F12 2D reporting is programmed, and a 30 s tap poll runs later. Touch is
**not yet wired into HID**, so SpringBoard still ignores the panel.

## Where the boot stands (2026-11-17)

* 0204 fixed the last early-probe `printf()` (`%s%s` with one argument). The
  boot now passes the white band and reaches the **four-bar test**, which is
  painted at the end of the storage init.
* The 0204 ring was pulled: **new log, total 27885 bytes, flush #188**. The
  early probe ran at 64.7 s kernel time, reset the DSX, walked the PDT and
  found the F12 record (`4c 00 13 06 01 12` at `0x00dd`), but rejected the F01
  record (`26 0d 04 01 01 81`, expected `00 06 0d 11`), so
  `dsx_f12_program()` bailed out and **reporting was never enabled**.
* The late F12 poll then read random bytes on every sample; the green band
  above the four bars was that noise, not a finger. The 300 s watchdog fired
  (`probe did not finish`) and reset the phone to fastboot.
* Everything else stayed healthy: CMD1 `OCR 0xc0ff8080`, 8-bit HS, GPT,
  HFSX mount, log flusher. One CMD24 timeout occurred while the poll ran.

## Next actions

1. **0205 searches for F01.** When the walk's F01 is not `00 06 0d 11`, it
   reads the raw `0x0000`..`0x0200` space in overlapping chunks and looks for
   the signature at every byte offset. Boot the image and read the ring for
   `F01 record at 0x....`.
2. Re-run and confirm `f12 ctrl28 <- 0x1f ack` plus a read-back.
3. Run the tap test only after that, then decode X/Y and feed
   `SweetHIDTouch::touch(finger, down, x, y)`.
4. Turn `sweetouchreboot` off while debugging so the phone stays put.

## Touch: what is known

* The part is the **Synaptics DSX/RMI4 at `0x20`**, not the Novatek variant.
* Early probe (before eMMC) is the only window where it answers; it resets
  the part, walks the PDT, finds F01 (`00 06 0d 11`) and F12, and programs
  F12 reporting (`RPT_DEFAULT 0x1F` to control 28). Touch data base:
  `F12 data base + data0 present`.
* F12 query block, from the 0201 ring:
  `01 01 01 02 00 00 00 0e 0f 15 c7 11 12 33 13 49 07 83 02 0b 81 04 03`
* The bit-banged I2C is byte-identical to the vendor driver (0188).
* Missing: raw samples from a finger-down event, so the 2D layout is still
  inferred rather than decoded.

## Build and CI

* Every kernel change needs a CI run (~10-15 min); there is no local
  `unixnu build`. The 0205 kernel (patch `87b7805`, workflow bump
  `66a8b93`) built green in run `44071902075`.
* Find the run for a commit:
  `curl -s "https://api.github.com/repos/N1709/ios-whyred/actions/runs?branch=whyred&per_page=1"`
* Fetch the kernel and pack a diag image:
  `env SWEETEARLY=1000 SWEETI2C=0 SWEETRESET=0 OUT_IMAGE=images/<name>.img bash tools/build-touch-diag.sh <run-id>`
* Boot arguments are listed in [`../README.md`](../README.md) and
  [`touch.md`](touch.md).

## Latest artifacts

| Image | Kernel | sha256 | Size |
| --- | --- | --- | --- |
| `shim-boot-touch-early-format.img` | `bb17acf` (0204) | `c4c197aa03be58f2bcd568079997396fcc297463c6d46d996d8ff5d77006cdb2` | 6,119,424 B |
| `shim-boot-touch-f01-scan.img` | `87b7805` + `66a8b93` (0205, run `44071902075`) | `a0ad381cc88396de3a8f1ce44d35e5ee0becf06e4f8dbf0962856218737593f6` | 6,119,424 B |
| `shim-boot-touch-emmc-phases-bright.img` | `31a73bd` (0203) | `acbdbcbbbb2a5254a49c090ac4d035020c0ba24ddefd7fed5992b06aca07965f` | 6,119,424 B |
| `shim-boot-touch-early-return.img` | `7afa32e` (0202) | `ca8ad3edfc7baca4b3f401c95b31700ad071f728f89f229e659e885768a2cfdf` | 6,119,424 B |
| `shim-boot-touch-emmc-phases.img` | `02fce27` (0201) | `bdcdbb8a747c313f08af2c1833d66894538c4b8f3fd36c67420896d6e52e7724` | 6,119,424 B |
| `shim-boot-touch-early-console.img` | `e180c4e` (0199) | `524a89e134f2a7ead15b6fd79690e729ae7006a398a8c8751f1363b64f1d8078` | 6,119,424 B |
