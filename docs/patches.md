# XNU patch series

Applied in order by CI. `sweet` patches are 0001-0079;
santoni/lavender/whyred patches start at 0080. Each row is one patch and
the reason it exists. Tests for every row live in `tests/run_tests.sh`.

| # | What |
| --- | --- |
| 0142 | SDHCI eMMC 5.1 driver for the Qualcomm SDCC v5 controller |
| 0143 | Persistent console log on the SYSTEM partition |
| 0144 | 4 KiB page support for the 16 KiB-signed dyld shared cache |
| 0145 | LAVENDER machine config (`ARM64_BOARD_CONFIG_LAVENDER`, 16K GICv3 board) |
| 0146 | Disable topaz MDP/DSI MMIO on lavender |
| 0147 | SDCC v5 takeover and BSD-root mount without the IOBSD 30 s stall |
| 0148 | WHYRED machine config; reuses the LAVENDER board code and enables the built-in libpthread/HFS/BearSSL providers |
| 0149 | Report the failing SDHCI probe step, CMD1 result and controller state |
| 0150 | Keep ABL's working card after a stuck `RESET_ALL` on SDCC v5 |
| 0151 | Keep ABL's bus width and name data-phase failures |
| 0152 | Read EXT_CSD in 1-bit after CMD0, print CMD8's R1 and the settled bus |
| 0153 | Try EXT_CSD at 1-bit and 8-bit through PIO and SDMA, dump the transfer registers |
| 0154 | Probe the first data phase across width/engine/preset and keep the winning setup |
| 0155 | Poll `PRESENT_STATE` for data-ready and transfer-complete; this core does not latch those bits in `INT_STATUS` |
| 0156 | Fix `PRESENT_STATE[11:8]` (active = 9/8), drain a stuck `DAT_INHIBIT`, keep `AUTO_CMD12` paths, skip CMD5 in TRAN |
| 0157 | Try SDMA before PIO for EXT_CSD, reject all-zero FIFO blocks and validate REV/STRUCTURE/SEC_COUNT before keeping a setup |
| 0184 | Bit-bang BLSP1 I2C on TLMM GPIO 2/3 and scan the touch addresses (found `0x20`) |
| 0185 | Paint the probe answer into the scanout buffer and halt; the console is muted at the framebuffer hand-off |
| 0186 | RMI4 identification at `0x20`: version, function table, F01/F12 query blocks (read-only) - wrong protocol |
| 0187 | Synaptics DSX protocol (page select at `0xFF`, PDT at `0x00e9`) plus an F12 data poll - still a shifted stream |
| 0188 | Open-drain bit-bang with clock-stretch wait, DT reset timing, read matrix, IRQ edge witness |
| 0189 | Seven address framings tried against `0x00e9`, auto-selected by repetition, plus a bootloader-mode reset retry |
| 0190 | BLSP1 QUP1 hardware I2C master (vendor v2 tag protocol, GCC `0x100000` RCG/branches, TLMM `blsp_i2c1` function index 3) behind `sweeti2c=1`, bit-bang kept as fallback; `sweetouchreboot=1` writes the IMEM bootloader reason and drops PS_HOLD so the diag boot returns to fastboot, with a kernel-thread watchdog (`sweetouchwatchdog`, 240 s) armed before the first clock write so a stalled GCC/QUP window still ends in fastboot |
| 0191 | Cache the DSX page and send `FF <page>` only when it changes, matching the vendor driver; the first addressed read after a reset is the only one that lands and later reads stream forward from the previous length |
| 0192 | Read a whole 128-byte page instead of guessing addresses, and place the F12 record inside it |
| 0193 | Spend the one landing read on a 256-byte PDT window from `0x00E9`, print both candidate function bytes, retry once after reset, poll F12 |
| 0194 | Read 24 bytes (four PDT records), force the page select before the read, parse `query/ctrl/data/fn` from the LSB, accept the F12 record at any offset, run E9/E3/E9 to test whether `FF <page>` re-arms the pointer, poll F12 at `(page << 8) \| data`, fix `dsx_identify` offsets and stop pulsing GPIO 66 (the Novatek reset) |
| 0195 | Leave the wakeup-gesture (LPWG) state the vendor `resume()` way: add an addressed `dsx_write_f()`, send no page select, spend the one landing read on F01 control `0x0D`, write `(v & ~0x07u) \| 0x80u` back, re-scan the PDT, dump F12 data/query/ctrl, and send the F01 normal-operation write before every F12 sample instead of `FF <page>` |
| 0196 | Issue the vendor software reset (`F01 command 0x06 <- 0x01`, 300 ms) after the write was proven to ACK in 0195, then walk the PDT downwards in addressed six-byte records with a forced page select, take the F01/F12 bases from the records, configure F01 only after the scan, and page-select before every F12 sample |
| 0197 | Probe inside the only window where the DSX answers: add `santoni_touch_early()` at the top of `santoni_sdhci_init()` (before eMMC and the framebuffer), wait until `sweetouchearly=MS` kernel time, run the 0196 reset + PDT walk there, print the real timestamps, cache the F12 bases for the late poll/touch test, and add `SWEETEARLY` to `tools/build-touch-diag.sh` |
| 0198 | Parse F12 `query_5`/`query_8`, refuse to program unless F01 is the real `00 06 0D 11` record, write `RPT_DEFAULT (0x1F)` to control 28, and poll the 2D block at `data_base + data0 present` - the vendor reference sequence, not the fork's `0x01`/all-data-registers variant |
| 0199 | Paint one framebuffer band per early-probe step (red/orange/yellow/green/cyan/blue/magenta/white) plus a matching `early mark N` line, because the SYSTEM ring is only flushed after eMMC init and a hang before it leaves no log at all; the bands map their own 2.6 MiB window because the boot `v_baseAddr` is physical, not a pointer |
| 0200 | Split the one band that never appeared into the steps inside `dsx_reset_probe()`: reset command, F01 status, each of the first six PDT records, the F12 block reads and the return, bands 7..19, painted only while the early probe sets `dsx_early_marks_on` so the late probe never repaints them |
| 0201 | Paint the eMMC phases that follow the early touch probe below the touch bands (early return, DT/maps/DMA, take-over entry, HC mode, reset/power/clock, CMD1, take-over return, 8-bit HS verify, partition, before/after `santoni_log_start()`), because the ring only starts in the last of them and a boot that hangs before it otherwise shows nothing above the white band; `santoni_touch_mark()` is a no-op unless the early probe ran |
| 0202 | Map the whole framebuffer once and paint a lime band in the gap after the early probe's last `printf()` but before its `return`, because the 0201 boot still stopped at the white band: either the probe never returned or the second window never mapped, and one window plus a return band separates those two |
| 0203 | Make every eMMC phase band bright and split take-over into entry, core check, HC mode, pre-reset snapshot and reset/power/clock, because the boot ended at the lime return band while the first phase colours (navy/olive/dark green) read as black on the panel, so a hang inside take-over looked the same as no phase band at all |
| 0204 | Fix the early probe's final `printf()`: the format had two `%s` but one string argument, so the second `%s` read the next register (value 1) and `vprintf_internal` took a data abort at `far 0x1` right after the white band - the "hang" every boot showed; print the F12 state with one `%s` and leave the rest to the existing separate lines |
| 0205 | Find F01 by signature instead of by walk alignment: the 0204 ring showed a perfect F12 record at `0x00dd` above an entry that is not `00 06 0d 11`, so the gate refused to program reporting and the poll read noise; when the walk F01 is wrong, read the raw `0x0000`..`0x0200` space in overlapping chunks, search every byte offset for the four signature bytes, and adopt the address that matches |
| 0206 | Take the F01 command base from the PDT stream: the 0205 ring shows the part ignores the address phase and streams F34 `27 00 09 00 21 34`, F01 `2f 26 0d 04 01 01` and F12 `4c 00 13 06 01 12`, so the reset goes to `0x26` instead of F12's data register `0x06`; read two PDT registers twice to prove the address phase, walk and program F12 only when it works, and use the cached page for every later read |
| 0207 | Default the panel to 60 Hz, notify CoreAnimation only on flush ticks, log the first `sweetfblog` swaps and every layer-set change, and time both `present()` paths against the 16667 us vsync budget |
| 0208 | SDM660 PM660/PM660L keys over the SPMI arbiter v2/v3: PON power/volume-down, PM660L GPIO7 volume-up with an input-enable check, and start the poller from `santoni_sdhci.c` (`sweetkeys=0` disables it) |
| 0209 | Skip the 30 s IORTC wait on whyred, read the PM660 RTC (`PPID 0x060`, `RTC_RDATA0..3`) through the same arbiter, and fall back to `sweetrtc=<unix seconds>` / `sweetrtcoff=<seconds>` when the raw counter is not Unix time |
| 0210 | Add the SDCC v5 exact-PIO/cache-state work, the post-write DAT0/CMD13 wait, the DMA diagnostics and the ADMA2 read path; reads use ADMA2 only after a PIO-vs-ADMA2 self-test, writes stay PIO, and failures fall back automatically (`sweetemmc_dma=0` forces PIO) |
| 0211 | Add `whyred_gpu.c`, the Adreno 512 ladder: read GCC/GPUCC state, vote the GPU BIMC clocks, bring up the CX/GX GDSC, force the GFX3D RCG root on XO, put the LDO in BHS mode, pulse the GX/AON resets and query the TrustZone GPU zap shader (PAS 13); off unless `whyredgpu=N` |
| 0212 | Add read-only `santoni_emmc_read()` for the audio/modem loaders and parse the GPT `modem` entry into `santoni_emmc_modem_lba()`; the fenced write path stays private |
| 0213 | Add `whyred_audio.c`: map LPASS Q6/SMMU/codec/pinctrl/AVTIMER/SMEM, vote the ADSP GDSC before the SMMU branch, authenticate the ADSP through PAS and read its firmware from the modem FAT16 `IMAGE/` directory; off unless `whyredaud=N` |
| 0214 | Add `whyred_wlan.c`: probe WCN3990 through the ANOC2 SMMU (SIDs `0x1a00`/`0x1a01`), read icnss/QUP/TLMM state; read only, off unless `whyredwlan=1` |
| 0215 | Add `whyred_modem.c`: enable the MSS clocks, program RMB, reset QDSP6, load PBL/MBA from the modem FAT, then authenticate the MDT and its segments through SCM PAS; off unless `whyredmodem=N` |
| 0216 | Add the `sweetnopb=1` lever: exec of `/Applications/PosterBoard.app/PosterBoard` fails with `EPERM` and logs `SWEET EXEC: refusing PosterBoard`, so one boot can tell whether the lock-screen freeze follows the poster process; off by default |

The earlier 0080-0141 topaz/santoni patches remain in the series for the
other devices.

