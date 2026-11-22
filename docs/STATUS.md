# Current status

Last updated: 2026-11-22. Patches `0207`-`0215` (display 60 Hz/telemetry,
PM660 keys, PM660 RTC, ADMA2 reads, GPU/audio/WLAN/modem drivers) are
committed locally and not built or booted. Last verified kernel/image:
`23e59b3` (patch 0206), `shim-boot-touch-pdt-reset.img`.

## One-paragraph summary

The kernel boots from `fastboot boot` into our XNU, brings up eMMC at 8-bit
high-speed, parses the GPT, mounts the HFSX iOS root read-write, starts the
console log flusher on SYSTEM and paints the four-bar framebuffer test.
The **touch controller** (Synaptics DSX at I2C `0x20`) is probed before eMMC,
its F12 2D reporting is programmed, and a 30 s tap poll runs later. Touch is
**not yet wired into HID**, so SpringBoard still ignores the panel.

The four large SDM660 drivers (GPU, audio, WLAN, modem) are now whyred
sources named `whyred_*`, each behind its own boot arg and off by default.
None has run on the phone yet.

## Where the boot stands (2026-11-19)

* The **0205 ring** answered the question: this part ignores the address
  phase after the first read and streams its register map. Reconstructed,
  the 536-byte stream is F34 `27 00 09 00 21 34`, **F01
  `2f 26 0d 04 01 01`**, F12 `4c 00 13 06 01 12`, an end marker, then the
  same bytes again.
* So the real F01 command base is **0x26**, not the RMI4 default `0x06`.
  Every reset since 0196 wrote `0x01` to `0x06`, which is F12's **data**
  register; the chip never left LPWG/suspend. The F01 control write also
  went to the walk's garbage `0x0083`.
* The `00 06 0d 11` literal does not exist in that stream, so gating on it
  rejected the only valid PDT this unit returns.
* **0206** parses the stream first (F12 record by full six bytes, nearest
  F01 record with control `0x0d`), resets at the record's command base,
  verifies the address phase by reading two PDT registers twice, and only
  then walks,
  programs F12 and polls. The RMI4 `0x06` base is tried as a fallback so one
  boot distinguishes both candidates.
* eMMC, GPT, HFSX mount, log flusher and the four-bar test all stayed
  healthy through 0204/0205.

## Next actions

1. **Build 0211-0215 in CI** and boot once with no new boot arg: all four
   probes must stay silent. Then enable one at a time and read the
   `WHYRED GPU/AUDIO/WLAN/MODEM` lines and the `"modem" first block` line.
2. **Boot 0206** and read the ring for `pdt stream F01 ... cmd 26`,
   `addr test`, `address phase works`, `f01 cmd 0x0026 <- 0x01` and
   `f12 ctrl28 <- 0x1f`.
3. Confirm the ctrl28 read-back, then run the tap test.
4. Decode X/Y and feed `SweetHIDTouch::touch(finger, down, x, y)`.
5. Keep `sweetouchreboot` off while debugging so the phone stays put.

## Touch: what is known

* The part is the **Synaptics DSX/RMI4 at `0x20`**, not the Novatek variant.
* Early probe (before eMMC) is the only window where it answers; it parses
  the streamed PDT, resets the part at the record's F01 command base (`0x26`
  on this unit), and programs F12 reporting (`RPT_DEFAULT 0x1F` to control
  28). Touch data base: `F12 data base + data0 present`.
* F12 query block, from the 0201 ring:
  `01 01 01 02 00 00 00 0e 0f 15 c7 11 12 33 13 49 07 83 02 0b 81 04 03`
* The bit-banged I2C is byte-identical to the vendor driver (0188).
* Missing: raw samples from a finger-down event, so the 2D layout is still
  inferred rather than decoded.

## Build and CI

* Every kernel change needs a CI run (~10-15 min); there is no local
  `unixnu build`. The 0206 kernel (`23e59b3`) built green in run
  `44506155564`.
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
| `shim-boot-touch-pdt-reset.img` | `23e59b3` (0206, run `44506155564`) | `4b55e7916dbda67c8835ecfefa105fdf4177862dd7954b1838a56636c9a42139` | 6,119,424 B |
| `shim-boot-touch-f01-scan.img` | `87b7805` + `66a8b93` (0205, run `44071902075`) | `a0ad381cc88396de3a8f1ce44d35e5ee0becf06e4f8dbf0962856218737593f6` | 6,119,424 B |
| `shim-boot-touch-emmc-phases-bright.img` | `31a73bd` (0203) | `acbdbcbbbb2a5254a49c090ac4d035020c0ba24ddefd7fed5992b06aca07965f` | 6,119,424 B |
| `shim-boot-touch-early-return.img` | `7afa32e` (0202) | `ca8ad3edfc7baca4b3f401c95b31700ad071f728f89f229e659e885768a2cfdf` | 6,119,424 B |
| `shim-boot-touch-emmc-phases.img` | `02fce27` (0201) | `bdcdbb8a747c313f08af2c1833d66894538c4b8f3fd36c67420896d6e52e7724` | 6,119,424 B |
| `shim-boot-touch-early-console.img` | `e180c4e` (0199) | `524a89e134f2a7ead15b6fd79690e729ae7006a398a8c8751f1363b64f1d8078` | 6,119,424 B |
