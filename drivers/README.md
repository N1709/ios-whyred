# drivers/

One folder per subsystem, and inside it one folder per driver. Two kinds of
source live here:

- **Domain mirrors** (`storage/`, `display/`, `gpu/`, `wifi/`, `touch/`,
  `audio/`, `power/`) hold the code this port actually runs, reconstructed
  from `xnu/patches/`, plus the hardware notes for each subsystem.
- **Driver sources** live in the subsystem's subfolder: `audio/dsp`,
  `gpu/adreno`, `touch/{dsx,input,hid}`, `power/{pmic,pmgr,cpu}` and
  `key-store/keystore`. They come from the UniXNU-whyred tree and are not
  build inputs here yet; each folder's README says what it covers and which
  part still has to be implemented.

Display and storage have **no driver sources here**: they use the kernel's
built-in drivers (`AppleCLCD`/`sweetfb` and `santoni_sdhci.c`) through the
patch series, so a second copy would only drift.

## The patch series is canonical

XNU only builds files at paths it knows, and this port changes XNU through
`xnu/patches/*.patch`. Editing a mirror under `drivers/` would be lost on the
next build.

`tools/extract_drivers.py` reconstructs every in-tree driver file from the
patch series and writes the mirror here:

```
python3 tools/extract_drivers.py          # refresh the mirrors
python3 tools/extract_drivers.py --check  # verify they match the patches
```

`tests/run_tests.sh` runs `--check`, so a patch and its mirror cannot drift.
When a driver folder becomes a build input it arrives through a numbered
patch like every other driver.

## Domains

| Folder | Hardware | Mirrored source | Driver source | State |
| --- | --- | --- | --- | --- |
| `storage/` | SDCC v5 eMMC `0x0c0c4000` | `santoni_sdhci.c` | none (kernel built-in) | working (`0142`+) |
| `display/` | `AppleCLCD` + ABL framebuffer `0x9d400000` | `SweetFramebuffer.cpp`, `SweetSurface.cpp` | none (kernel built-in) | scanout works; P3 mode awaiting hardware verification |
| `gpu/` | Adreno 509 (`0x05000900`) | `whyred_gpu.c`, `WhyredGPU.cpp` | `gpu/adreno` | ladder/service compiled, off by default, not hardware-tested |
| `wifi/` | WCN3990 (SNOC) + BT SLIMbus | `whyred_wlan.c`, `TopazModem.cpp` | none yet | platform/SMMU probe compiled; no WLAN data path |
| `modem/` | SDM636 MSS/QDSP6 | `whyred_modem.c` | none yet | firmware/PAS ladder compiled, off by default, not hardware-tested |
| `touch/` | Synaptics DSX / Novatek NT36672 (I2C) | `whyred_touch.c`, `sweet_touch.c` | `touch/dsx`, `touch/input`, `touch/hid` | probing (`0184`+) |
| `audio/` | WCD9335 + ADSP | `whyred_audio.c` | `audio/dsp` | ADSP loader compiled, off by default; no CoreAudio/codec path |
| `power/` | PM660/PM660L charger and gauge | `WhyredBattery.cpp`, `whyred_power.c` | `power/pmic`, `power/pmgr`, `power/cpu` | static battery only; real gauge/charger disabled |
| `key-store/` | board-independent key events | none in tree | `key-store/keystore` | not started |

Hardware facts for the domains with no code yet are in `gpu/hw.md` and
`audio/hw.md`; the board DTS values come from `refs/whyred-android/`.

## Before writing a new driver

1. Read the domain README, then `THIRD_PARTY.md` for the hardware match.
2. Check whether the reference addresses the same silicon. This board is in
   the SDM660/SDM636 family, but every register and rail name still has to be
   checked against `dts/whyred.dts`.
3. Write the driver at a path XNU builds, through a numbered patch, and mirror
   it with `tools/extract_drivers.py`.
