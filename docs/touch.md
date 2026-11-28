# Touch (whyred)

## Hardware

* Controller: **Synaptics DSX / RMI4** at I2C address `0x20` on BLSP1 QUP1
  (`0x0c175000`, GIC SPI 95). Some whyred panels ship a Novatek NT36672 that
  answers at `0x62` while its bootloader runs and `0x01` after firmware load;
  the probe scans the whole bus, but this unit is the DSX.
* SDA GPIO 2, SCL GPIO 3 (TLMM SOUTH tile, `+0x100000`); IRQ GPIO 67, and
  GPIO 66 is the **Novatek** reset, not the DSX's (TLMM NORTH tile,
  `+0x900000`).
* The vendor DT for the Synaptics variant
  (`sdm636-touchpad-whyred.dsi`) comments out `synaptics,reset-gpio` and has
  no power GPIO: the DSX is reset only by the F01 command or by its rail.
  `ts_active` configures GPIO 66/67 as function `gpio` with a pull-up and no
  value.
* Rail: `vcc_i2c` = `pm660_l11`; it also powers the bus pull-ups. The board
  has no external pull resistors.
* The chip only answers in a **window during early boot** (before eMMC and
  before the QUP driver takes the bus), so the probe runs there.

## Latest hardware result (2026-11-23)

The P3/e92f698 ring proves the late LDO11 path is safe and active on this
unit: APID 39 is APPS-owned, enable reads `0xa0`, and both disable and enable
ACK. The subsequent address test still advances through the byte stream,
including after F01 reset at `0x26` and the fallback reset at `0x06`:
`address phase still ignored (streaming)`. Therefore F12 programming, its
poll and the HID thread are correctly skipped; there are no phantom touch
events. The current short rail cycle did not solve addressing. The next
isolated diagnostics are a fully timed rail cycle, the existing general-call
reset and QUP framing, followed by F01/F12 setup only if `dsx_addressable`
becomes true. Recovery stays read-only.

One caveat remains before calling the rail experiment electrically complete:
the current code waits only 20 ms after disable and 50 ms after enable, and
does not read `ENABLE_CTL` while off. The vendor DT's power/reset timing is
200/200/20 ms. A later touch-only diagnostic should read back bit 7 after
disable, hold the rail off for 200 ms, restore it, wait 200 ms, then run the
address test. Do not mix that timing change into the P3 display boot.

## What the probe does

`whyred_touch_early()` runs at the top of `santoni_sdhci_init()`:

1. Waits until `sweetouchearly=MS` kernel time (default off, diag images use
   `1000`).
2. Maps TLMM, sets up SDA/SCL, clears the bus.
3. Does **not** touch PM660 LDO11 here. `whyred_touch_early()` runs before
   `santoni_log_start()`, so an SError from the SPMI arbiter would panic the
   boot with nothing written to the ring. With `sweetouchpower=1` the rail
   toggle happens later, in the late probe, where a fault leaves
   `WHYRED TOUCH:` lines behind. On whyred the vendor DTS declares LDO11 as
   an RPM-managed regulator (`qcom,rpm-smd-regulator`), so the APPS usually
   does not own its APID; the late probe reads the arbiter ownership table
   and refuses the toggle (`owner EE<n>, not APPS; not toggling`) instead of
   writing another EE's rail. The normal image therefore uses the bit-bang
   path only.
4. Bit-bangs I2C at ~50 kHz (byte-identical to the vendor driver, 0188).
   A high is a release plus a wait for the line to rise; the wait is 500 us
   since 0223 (the old 2 ms wait made a stretched bit expensive), and
   `sweetouchscl=<us>` overrides it. With `sweeti2c=1`, the scan hands the
   bus to the BLSP1 QUP1 hardware master afterwards; 0227 makes the QUP
   write side usable as well as the read side.
5. Reads the streamed register map in two back-to-back `0x200` windows (the
   cycle is 536 bytes and the pointer position is unknown). This part ignores
   the address phase after the first read, so the PDT records arrive back to
   back: F34, F01 `2f 26 0d 04 01 01`, F12 `4c 00 13 06 01 12`. 0206 takes
   the F01 command base (`0x26`) from that record; the RMI4 default
   `00 06 0d 11` does not exist on this unit. `tools/dsx-pdt.py <ring>`
   reports the triplet and the stream phase. With `sweetouchfast=1` (0223)
   the two reads are skipped and those known bases are used directly; the
   address test below still runs, so a wrong guess cannot write F12.
6. Software-resets the part at that command base, then reads two PDT
   registers twice to test whether the address phase works. Only an
   addressable part is walked and gets F12 programmed; a still-streaming part
   tries the `0x06` base once more first. The streamed F12 bases are **not**
   written to: while `dsx_addressable == 0`, `ctrl20`/`ctrl28` writes would
   land on whatever register the stream pointer is on (0222). With
   `sweetouchpower=1`, 0224 toggles LDO11 after this and repeats the address
   test once, so one boot distinguishes "the rail cycle helped" from "still
   streaming".
7. Clears the F01 sleep bits and the F12 `ctrl20` wakeup-gesture byte
   (`synaptics_rmi4_f12_wg(false)`), then programs F12 reporting:
   `RPT_DEFAULT (0x1F)` to control 28. When the address phase works, 0225
   also forces `NO_SLEEP_ON` (F01 control bit 2) so the part cannot drop back
   into LPWG on its own. The 2D data base is
   `F12 data base + data0 present`. `ctrl28` alone only selects the report
   type; a part left in LPWG keeps its 2D block inactive until the gesture
   byte is cleared, which is the state Android hands over on a warm reboot.
   This whole step is skipped unless the address phase works. On the QUP
   path, 0227 sends the page select as its own write and the register plus
   data as a second write; before it, every F12 write returned `EIO` once
   the QUP owned the bus.
8. Returns. The late probe (`IOKitBSDInit`) reuses the PDT result,
   re-attempts F12 programming if the early pass failed, and runs a
   `sweetouchtouch` second poll of the F12 2D block. The 12-sample poll, the
   `sweetouchtouch` test and the HID thread all refuse to run while the part
   still streams (0222). With `sweetouchpower=1` this is also the only place
   that toggles LDO11.
9. After the probe is complete, 0226 forwards every display power change from
   `SweetFramebuffer` to `whyred_touch_display_power()`. Display off pauses
   the HID poll, waits for an in-flight read, and writes F01 `SENSOR_SLEEP`
   (bit 0). Display on writes `NORMAL_OPERATION | NO_SLEEP_ON`, programs F12
   again, and resumes the poll. A part that never reached the address phase is
   left alone.

With `sweeti2c=1` the QUP is the bus master. 0190's transport always accepted
write-only transfers, but the DSX layer refused them; 0227 closes that gap
(page select, then register plus one to three data bytes). The HID poll asks
for ten 8-byte records, larger than the QUP FIFO's documented minimum of 32
bytes plus two echo bytes, so on QUP it reads three records per transaction;
the bit-bang path is unchanged. Neither the QUP write path nor the chunked
poll has run on the phone.

The F12 query block read on this unit:

```
01 01 01 02 00 00 00 0e 0f 15 c7 11 12 33 13 49 07 83 02 0b 81 04 03
```

## Boot arguments

| Argument | Effect |
| --- | --- |
| `sweetouchi2c=1` | run the touch probe (bus scan + chip-ID reads) |
| `sweeti2c=1` | after the bit-bang scan, make BLSP1 QUP1 the bus master; reads always, writes since 0227; opt-in diagnostic |
| `sweetouchearly=MS` | run the whole probe at `MS` kernel time, before eMMC |
| `sweetouchtouch=SEC` | poll the F12 2D block for `SEC` seconds after the late probe (max 60) |
| `sweetouchscl=US` | bit-bang line-rise wait in microseconds (default 500, clamped 10..20000; 0223) |
| `sweetouchfast=1` | skip the two `0x200` stream reads and use this unit's known F01/F12 bases; the address test still runs (0223) |
| `sweetouchreset=1` | drive reset GPIO 66 per the DT (low 20 ms, high 200 ms) |
| `sweetouchwait=SEC` | hold the panel with the probe output for `SEC` seconds |
| `sweetouchreboot=1` | reboot to the bootloader when the probe finishes; also arms the reboot watchdog, because this is the only boot that intentionally leaves XNU |
| `sweetouchwatchdog=SEC` | watchdog delay for `sweetouchreboot=1` (default 300 s, `0` disables). The probe cancels it once it finishes, so a normal or log boot is never reset |
| `sweetouchpower=1` | late probe only: toggle PM660 LDO11 (`vcc_i2c`) through the SPMI arbiter, after refusing an APID another EE owns; off by default, a fault leaves `WHYRED TOUCH:` lines in the ring |
| `sweetouchprereset=1` | before the first read, write the F01 device reset to `0x06` and `0x26` and the wake value to F01 control `0x0d`; the only ordering where a reset can still land on a part that streams after its first read |
| `sweetouchgap=US` | post-read gap in microseconds before the next address phase (default 0, max 20000; 0246) |
| `sweetouchgapsweep=1` | one boot tries 0/100/500/1000/5000 us and logs which gap makes the second addressed read repeat the first; read-only (0246) |
| `sweetouchnosleep=0` | do not force `NO_SLEEP_ON` when the panel is on (0225; the default is to force it) |
| `sweetouchgcall=1` | try the I2C general-call reset (address `0x00`, byte `0x06`) when the address phase is ignored (0225; opt-in because it reaches every device on BLSP1 QUP1) |

## Display power and the power button

The power button already delivers `key=0` to `SweetHID`; what stopped the
screen from blanking was the framebuffer's `sweetfb_stayon=1` veto, which
ignored **every** display-off request. 0226 keeps the veto only until the
first userspace present (`sweet_fb_present_logged == 0`). After that,
backboardd's `RequestPowerChange` sets `fPowerOff` and calls
`whyred_touch_display_power()`.

whyred inherits the LAVENDER board config, so the DSI/MDP block is compiled
out and `fDsi == NULL`: there is no DCS panel-off command. Instead the vsync
timer takes the in-place fade path, halves every channel in the boot
framebuffer six times, writes black and idles; display-on marks the frame
dirty and the next present composes normally. 0228 also drops presents while
`fPowerOff` is set: the 2026-11-22 ring kept presenting after display-off
(744 presents after its second request), which would have repainted the fade.
The latest ring shows the transition and no presents after display-off, but
the SpringBoard gamut crash already leaves the screen black, so visual
blank/wake behavior is not yet a clean hardware verdict.

Expected ring lines:

```
WHYRED TOUCH: f01 ctrl 0x84 -> 0x81 (sleep)
WHYRED TOUCH: f01 ctrl readback 0x81
WHYRED TOUCH: display off, HID paused, DSX asleep
...
WHYRED TOUCH: f01 ctrl 0x81 -> 0x84 (normal)
WHYRED TOUCH: f01 ctrl readback 0x84
WHYRED TOUCH: display on, HID resumed, DSX awake
```

`0x84` is `CONFIGURED | NO_SLEEP_ON`; `0x81` is `CONFIGURED | SENSOR_SLEEP`.
If the panel is blanked before the touch probe finishes, the hook is a no-op:
there is nothing addressable to suspend yet.

The default build no longer passes `sweetfb_native=1`. iOS now sees the
iPhone X canvas (1125x2436) and `SweetFramebuffer` crops it to the 1080x2160
panel (`fOffX=20`, `fOffY=138`), which removes UIKit's red
"rdar:45025538" clock. `SWEETFB_NATIVE=1` is the opt-in path back to the
native panel canvas and the red fallback.



The SYSTEM ring only starts after eMMC init, so the early probe cannot be
read from a log. It paints coloured bands into the framebuffer instead, and
`whyred_touch_mark()` continues the map for the eMMC phases that follow.

Bands 1–19, top to bottom, starting at row 40 with 70-row step:

| # | Colour | Step |
| --- | --- | --- |
| 1 | red | probe entered |
| 2 | orange | reboot watchdog armed (`sweetouchreboot=1` only; 0222) |
| 3 | yellow | past the time wait |
| 4 | green | TLMM mapped |
| 5 | cyan | pins set |
| 6 | blue | bus cleared |
| 7..17 | maroon, purple, grey, salmon, light yellow, pink, light cyan, violet, light grey, dark grey, teal | reset, F01, PDT records, F12 reads |
| 18 | charcoal | `dsx_reset_probe()` returned |
| 19 | white | F12 programmed, early probe done |

A lime band at rows 1425..1445 is painted after the probe's last `printf()`
and before its `return` (0202), so white + lime means the probe really
returned.

Phase bands from row 1450, 54-row step, 40 rows tall:

| Colour | Step |
| --- | --- |
| magenta | early probe returned to storage init |
| cyan | DT, mappings and DMA buffer ready |
| yellow | take-over entry |
| green | core check + pre-write snapshot |
| orange | HC mode entered |
| red | pre-reset snapshot |
| blue | reset/power/clock done |
| violet | CMD1 answered |
| pale cyan | take-over done |
| pink | 8-bit high-speed verified |
| white | GPT partition found |
| grey | before `santoni_log_start()` |
| brown | ring started |

The last visible band names the step that did not finish.

## Known open items

* No finger-down samples have been captured yet, so the F12 2D report layout
  is not decoded.
* GPIO 66 is the Novatek reset; the DSX has no reset line in the vendor DT.
  Do not expect a GPIO pulse to recover it.
* Next read-only recovery probe: address `0x2c` (the DT's UB address) and a
  general-call `00 06`, to tell bootloader mode from a bus-resettable state.
* `drivers/touch/hid/native/NativeHidInput.cpp` already exposes a report-3
  digitizer descriptor and `native_touch_*` sink globals;
  `SweetHIDTouch::touch(finger, down, x, y)` is the path to use once X/Y are
  known.
* The Novatek node in the vendor DT is an unused variant for this unit.
