# Touch (whyred)

## Hardware

* Controller: **Synaptics DSX / RMI4** at I2C address `0x20` on BLSP1 QUP1
  (`0x0c175000`, GIC SPI 95). Some whyred panels ship a Novatek NT36672 that
  answers at `0x62` while its bootloader runs and `0x01` after firmware load;
  the probe scans the whole bus, but this unit is the DSX.
* SDA GPIO 2, SCL GPIO 3 (TLMM SOUTH tile, `+0x100000`); reset GPIO 66 and
  IRQ GPIO 67 (TLMM NORTH tile, `+0x900000`).
* Rail: `vcc_i2c` = `pm660_l11`; it also powers the bus pull-ups. The board
  has no external pull resistors.
* The chip only answers in a **window during early boot** (before eMMC and
  before the QUP driver takes the bus), so the probe runs there.

## What the probe does

`santoni_touch_early()` runs at the top of `santoni_sdhci_init()`:

1. Waits until `sweetouchearly=MS` kernel time (default off, diag images use
   `1000`).
2. Maps TLMM, sets up SDA/SCL, clears the bus.
3. Bit-bangs I2C at ~50 kHz (byte-identical to the vendor driver, 0188).
4. Software-resets the DSX, walks the PDT downwards with an addressed page
   select, and finds F01 (`00 06 0d 11`) and F12. If the walk's F01 is not
   the standard record, 0205 dumps the raw `0x0000`..`0x0200` space; rebuild
   it with `tools/dsx-pdt.py <ring>` to see where the table really starts.
5. Programs F12 reporting: `RPT_DEFAULT (0x1F)` to control 28; the 2D data
   base is `F12 data base + data0 present`.
6. Returns. The late probe (`IOKitBSDInit`) reuses the PDT result and runs a
   `sweetouchtouch` second poll of the F12 2D block.

The F12 query block read on this unit:

```
01 01 01 02 00 00 00 0e 0f 15 c7 11 12 33 13 49 07 83 02 0b 81 04 03
```

## Boot arguments

| Argument | Effect |
| --- | --- |
| `sweetouchi2c=1` | run the touch probe (bus scan + chip-ID reads) |
| `sweetouchearly=MS` | run the whole probe at `MS` kernel time, before eMMC |
| `sweetouchtouch=SEC` | poll the F12 2D block for `SEC` seconds after the late probe (max 60) |
| `sweetouchreset=1` | drive reset GPIO 66 per the DT (low 20 ms, high 200 ms) |
| `sweetouchwait=SEC` | hold the panel with the probe output for `SEC` seconds |
| `sweetouchreboot=1` | reboot to the bootloader when the probe finishes |

## Panel band map (diag images)

The SYSTEM ring only starts after eMMC init, so the early probe cannot be
read from a log. It paints coloured bands into the framebuffer instead, and
`santoni_touch_mark()` continues the map for the eMMC phases that follow.

Bands 1–19, top to bottom, starting at row 40 with 70-row step:

| # | Colour | Step |
| --- | --- | --- |
| 1 | red | probe entered |
| 2 | orange | reboot watchdog armed |
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
* `drivers/touch/hid/native/NativeHidInput.cpp` already exposes a report-3
  digitizer descriptor and `native_touch_*` sink globals;
  `SweetHIDTouch::touch(finger, down, x, y)` is the path to use once X/Y are
  known.
* The Novatek node in the vendor DT is an unused variant for this unit.
