# Build, boot and layout

All commands run from the repository root.

## How it boots

```
ABL (stock Xiaomi bootloader, unlocked)
  -> fastboot boot shim-boot.img        (Android boot image, RAM only)
     -> shim on Silver CPU0: prints MMFR0/MIDR, wakes Gold CPU 0x100 via PSCI, parks
        -> shim on Gold: GICv3 setup, timer unmask, locates payload
           -> XNU Mach-O + Apple Device Tree (ADT) + boot-args
              -> XNU -> SDHCI eMMC driver -> HFS+ root in userdata
                 -> dyld + shared cache -> launchd -> backboardd + SpringBoard
```

There is no serial console. The kernel draws its console into ABL's framebuffer; with `sweetfb_noconsole=1` it stops after the first iOS frame (panics are still drawn). The kernel also keeps sweet's persistent log ring at the end of `userdata` and its user stack sampler (`SMP name tN pc .. bt ..`).


## How to do it yourself

You need a Redmi Note 5 / Note 5 Pro (`whyred`, SDM636) with an
**unlocked bootloader**, a Linux PC with `fastboot`, and a GitHub account for
the kernel build. The current device already has its HFSX root inside the
existing `userdata`; do not recreate, format or flash it. Every kernel test
uses `fastboot boot` from cold fastboot. Boot, recovery, modem, persist,
firmware, bootloaders and the GPT are never written.

### 1. Build the kernel

The kernel builds on GitHub's `macos-15-intel` runner with the oldest Xcode installed on that image; `macos-14` retired on 2026-11-02, and the plain `macos-15` label now enters the high-demand M1 queue. The build uses [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build). Fork the repo and run, on the `whyred` branch:

```sh
gh workflow run xnu.yml -R YOU/ios-whyred --ref whyred
gh run download RUN_ID -R YOU/ios-whyred -n xnu-kernel -D xnu/out
```

The workflow defaults to `MACHINE_CONFIG=WHYRED`; pass
`-f machine_config=LAVENDER` to build the Redmi Note 7 kernel instead.

A build takes about 8 minutes. If the artifact upload times out on GitHub's side, rerun the job. Check that the kernel has no LSE left (`llvm-objdump -d --mattr=+v8.5a ... | grep -P '\t(cas|ldadd|ldset|ldclr|swp|ldapr)'`; the only hit should be the `K256` SHA table).

### 2. Build the iOS root image

Download the iPhone X (iPhone10,3) iOS 16.7.16 (20H392) IPSW from Apple. On Linux, with `apfs-fuse` (build it from [sgan81/apfs-fuse](https://github.com/sgan81/apfs-fuse) if your distribution has no package), `hfsprogs` and sudo:

```sh
# OS, SystemOS and AppOS disk images (see BuildManifest.plist, D22AP Erase identity)
sudo bash tools/mkiosroot.sh OS.dmg SystemOS.dmg AppOS.dmg ios16-root.img 16G
sudo mount -t hfsplus -o loop,force ios16-root.img /mnt/iosroot
sudo python3 tools/sweet-root-tweaks.py /mnt/iosroot
sudo umount /mnt/iosroot
img2simg ios16-root.img ios16-root.simg
```

The device tree builder copies `/product` from the real iPhone X device tree. Take `Firmware/all_flash/DeviceTree.d22ap.im4p` from the IPSW, decompress its LZFSE payload (`ipsw img4 im4p extract`) and save it as `research/ipsw-x/d22ap.adt`.

That Apple ADT is intentionally untracked. A GitHub runner can build the
kernel, but its packaged boot-image artifact has no complete D22AP `/product`
node and is **not** the P3 hardware-test image. Download the `xnu-kernel`
artifact and repack locally where `research/ipsw-x/d22ap.adt` exists.

### 3. Build the boot image

Needs `aarch64-linux-gnu-gcc` and AOSP `mkbootimg.py` (set `MKBOOTIMG` if it is not at the default path in `shim/build.sh`).

The stock ABL matches the appended DTB against `qcom,msm-id`/`qcom,board-id`
before it boots the kernel, so a whyred image needs the whyred DTB. Extract it
from any boot or recovery image built for the phone (stock MIUI `boot.img`,
TWRP/OrangeFox recovery, or a ROM `boot.img`):

```sh
# -> resources/whyred_dtb.bin (msm-id 0x159, board-id 0x30008, "SDM 636 ... E7S")
python3 tools/extract_appended_dtb.py twrp-whyred.img --board whyred
```

Put `kernel.development.whyred` from the CI `xnu-kernel` artifact in
`xnu/out/` (or pass its path: `BOARD=whyred shim/build.sh /path/kernel`), then:

```sh
BOARD=whyred shim/build.sh      # -> shim/out/shim-boot.img
```

The script runs `tools/mkadt.py --board whyred`, packs the kernel with
`tools/mkpayload.py` and appends `resources/whyred_dtb.bin` when present.
Before packing, `tools/check_payload.py` verifies that the kernel is an arm64
Mach-O and that the DTB carries this board's msm-id/board-id: a wrong-machine
kernel or DTB stops the build instead of failing later in fastboot.
Build the lavender image with `BOARD=lavender shim/build.sh`.

| Boot-arg | Effect |
| --- | --- |
| `rd=emmcroot0 emmcrw=1` | Existing HFSX root inside `userdata`, writable |
| `cpus=4` | All four Gold cores; `cpus=1` is the safe fallback |
| `sweetfb_noconsole=1` | Stop drawing kernel text over the GUI after the first iOS frame. Leave it out to debug (much slower: every console line is drawn and scrolled by the CPU) |
| `sweetfb_hz=60` | Panel refresh rate of the CPU frame copy (driver default is now 60) |
| `sweetfblog=N` | Log the first N presented frames (default 2000), then every 256th; `SWEET FB: present ... OVER` marks a frame over the 16667 us budget |
| `sweetkeys=0` | Disable the PM660/PM660L key poller |
| `sweetrtc=<unix seconds>` | Calendar base when the platform clock answers 0 |
| `sweetrtcoff=<seconds>` | Add this offset to the raw PM660 RTC counter when it is not Unix time |
| `sweetnopb=1` | Refuse to exec PosterBoard and log it, to isolate the lock-screen poster crash (0216) |
| `sweetemmc_dma=0` | Keep eMMC reads on PIO; ADMA2 is the default after its self-test |
| `whyredgpu=N` | Run GPU ladder step N after `whyredgpudelay` seconds (default 25); `30` starts the IOKit service after the ring is up, `31` also runs its self-test; off by default |
| `whyredmetal=1` | Publish `WhyredMetal`/`WhyredMetalDevice` on `IOAcceleratorES`; only with `whyredgpu=30` and the bundle installed |
| `whyredgpuclk=160\|266` | Switch the GFX3D PLL/RCG from XO to the given frequency; off by default, verify at step 4 |
| `whyredgpu_verbose=1` | Log every GPU service submission and buffer map |
| `whyredaud=N` | Run the ADSP audio step N after `whyredauddelay` seconds; `whyredaudforce=1` overrides its checks; off by default |
| `whyredwlan=1` | Probe WCN3990 after `whyredwlandelay` seconds (default 25); read only |
| `whyredmodem=N` | Run the MSS/QDSP6 step N after `whyredmodemdelay` seconds; `whyredmodemclk=<mask>` selects the MSS clocks; off by default |
| `sweettouch=0` | Disable the touch driver |
| `sweethw=1` | Also start sweet's SM7150 key and USB drivers (**unsafe on topaz**, not added) |
| `launchd_unsecure_cache=1`, `wdt=-1 wdt_test=no_user_checkin` | launchd without AMFI; no hardware watchdog |

### 4. Boot the temporary image

The CI kernel artifact is only input for `shim/build.sh`. Do not flash the
boot image, recovery, modem, persist, a bootloader, or `userdata`; do not
format or alter the GPT. The current whyred workflow starts the image from
RAM only:

```sh
fastboot -s "$WHYRED_SERIAL" boot shim/out/shim-boot.img
```

Use cold fastboot (phone off, Volume Down + Power); warm fastboot entered by
`adb reboot bootloader` rejects `boot` on this unit. Never reboot the phone
unless the user explicitly asks for that boot. Recovery is read-only and is
used only to pull the SYSTEM ring after a test.

The shim screen (red) shows `MMFR0`, the Gold hand-off and `SPEED silver=… gold=…` for 8 seconds, then XNU starts. whyred does not show an Apple logo: that logo is drawn by iBoot, which is not in this chain, and the kernel console is on the UART while `kPEAcquireScreen` clears the boot framebuffer. Until the display path is up the panel is black on purpose (see `NOTES.md`). To get back to fastboot, hold Power + Volume down.

 A manual run ("Run workflow") has `both`, `normal`, `log`, `quick` and
`console` choices; a push builds the quiet normal artifact. P3 hardware tests
are repacked locally as explained above:

| Image | Command line | Use |
| --- | --- | --- |
| `shim-boot.img` | no `sweetfbtest` (the four-bar scanout probe is opt-in), no `sweetlog`, no `cs_debug=2`, no kprintf spam | daily boot; nothing is recorded to the SYSTEM ring |
| `shim-boot-log.img` | the same plus `sweetlog=1 cs_debug=2 -enable_kprintf_spam` | when a line is missing; records the 4 MiB SYSTEM ring and every validated code page, several times slower at 115200 baud |

The log image writes the console to the 4 MiB SYSTEM ring, so its full boot can be pulled after a black-screen boot with `tools/sweet-log-pull.sh`. The normal image leaves the ring untouched and has no logging overhead.

`fb_test_pattern` (a manual-run choice, default `off`) adds `sweetfbtest=1`: four colour bars written into the boot framebuffer before BSD init. It was the scanout proof before the lock screen existed; turn it on only to check that MDSS still scans ABL's splash out.

### Touch (whyred)

Measured on this phone, not assumed: the controller answers at I2C **`0x20`
(Synaptics)** on BLSP1 QUP1, SDA GPIO 2 / SCL GPIO 3 (TLMM SOUTH), reset GPIO
66 and IRQ GPIO 67 (TLMM NORTH). The 0184 boot recorded `rst cfg 000003c3 io
00000003` (GPIO output, driven high - ABL hands it over out of reset), `irq
cfg 00000001 io 00000001` (input, idling high) and `idle sda 1 scl 1`: the
internal pull-up was enough for the bootloader-mode stream at ~50 kHz.
Novatek `0x62`/`0x01` does not answer
on this unit; the Tianma/Huaxing variants are where that address is expected.
The Shenchao variant is the Synaptics one.

Ring 0197 (2026-11-05) is the first framing matrix against the app-mode part
over XNU's bit-bang: `0x20` ACKs, but every framing reads
`00 00 00 00 00 00`. A raw probe against the recovery QUP reproduces the same
zeros while that driver is suspended (see `NOTES.md`), so the zeros are a chip
state, not yet a bit-bang verdict. The DT touch node has no
`synaptics,reset-gpio`, so the working kernel never pulses GPIO66 - XNU's
`sweetouchreset=1` is the one thing the two environments do not share. Build a
no-reset image with `SWEETRESET=0` before blaming the 5 kHz change.

The page table itself is no longer in doubt. A 150 s read loop on the recovery
QUP caught a coherent stream, and it segments into the real table:

    27 00 09 00 21 34   F34 (flash update)
    2f 26 0d 04 01 01   F01 (device control, command base 0x26)
    4c 00 13 06 01 12   F12 (2D touch, data base 0x06)
    00 00 00 00 00 00   end of table

So the controller, its firmware and F12 all exist. The same loop also shows
that 75 reads of the *same* address walk the table six bytes at a time on the
known-good QUP, which means the address write is ignored: the part is
streaming, not misaddressed, and the bit-bang is not at fault. The vendor
driver puts the part to sleep on suspend (`enable_wakeup_gesture is on`) at
3.25 s, and a warm reboot inherits that state - that is the `00` matrix of
rings 0197/0199. A cold boot into XNU, which never suspends the DSX, is the
open question; ring 0198 got real but unstable bytes because the part was
already streaming, not because the bus was mis-sampling.

One trap for anyone repeating this on the recovery side: unbinding `1-0020`
(or a failed rebind) runs the driver's power-off path and disables `vcc_i2c`,
which also feeds the external I2C pull-ups. Every read after that is random or
`ff` and says nothing about the chip; keep the driver bound.

The `novatek@62` node in the vendor DT is a variant entry, not this hardware.
Its reset GPIO 66 already sits high, and `0x62` does not ACK on any scan even
after a reset pulse, while `0x20` ACKs on the bit-bang scan every time. Do not
pivot the touch work to the Novatek protocol and do not pulse GPIO 66 again.


Patch `0184` (`bsd/dev/whyred_touch.c` since 0219) re-muxes the two bus lines as GPIO
and bit-bangs I2C, because neither a TLMM GPIO driver nor a QUP I2C master
exists in this kernel yet. `0186` guessed RMI4 (two-byte register address at
`0x000e`); `0187` switched to the protocol the vendor driver actually uses
(page select `0xFF <page>`, one address byte, table at `0x00e9`), and both
returned the same continuous byte stream, one that shifts with the read
history instead of with the address. `0188` therefore treats the read itself
as the suspect: it makes SCL open-drain with clock-stretch waiting, pulses
reset with the DT timings, and runs a read-only matrix (same address twice,
one vs two address bytes, `0x00e8`/`0x00ea`, page 1, no page select) plus an
IRQ witness on GPIO 67 during the F12 poll.

| Boot-arg | Effect |
| --- | --- |
| `sweetouchi2c=1` | run the probe: scan `0x08`..`0x77`, then dump the RMI4 identification |
| `sweeti2c=1` | after the address scan, bring up the BLSP1 QUP1 hardware master and read the DSX through it; the bit-bang remains the fallback if the clocks or the SE do not come up |
| `sweetouchreset=1` | additionally drive reset (GPIO 66) per the DT: low 20 ms, high 200 ms, then 50 ms settle before the scan |
| `sweetouchprereset=1` | before the first read, write the F01 device reset to `0x06` and `0x26` (300 ms each) and the vendor wake value `0x84` to F01 control `0x0d`; only useful on a part that streams after its first read |
| `sweetouchpower=1` | late probe only: toggle PM660 LDO11 (`vcc_i2c`) through the SPMI arbiter after checking that the APID is APPS-owned; off by default because a non-owned arbiter access raises the SError that panicked the pre-log probe |
| `sweetouchwait=SECONDS` | hold the boot after the scan (default 15 s, 0 = no hold, max 60); the panel is usually muted by then, so prefer the ring |
| `sweetouchtouch=SECONDS` | poll F12 touch data for that long: top band of the panel is blue while waiting, green on the first change or IRQ edge, and every change goes to the ring (max 60) |
| (automatic) | before identification, 0189 prints every address framing it knows (`page|a1`, `page+a1`, `page|a2be`, `page|a2le`, `a1`, `a2be`, `a2le`) against `0x00e9`, marks each one `addressed`, `streams` or `failed`, and keeps the first that returns the same six bytes twice with a different entry one slot above (`using frame ...`) |
| (automatic) | if the page table has F01 but no F12, 0189 treats the part as bootloader-mode, pulses reset once and scans again |
| `sweetouchhalt=1` | paint the answer into the scanout buffer and stop the boot there (top quarter: white = first ack `0x01`, yellow = `0x62`, cyan = `0x20`, magenta = other, red = none; bottom green if anything answered) |

Without `sweetouchreset` the probe does not write GPIO 66/67, so the log also
records the reset/IRQ pad state ABL handed over.

The controller state is not a software problem any more: an earlier session left
it parked in flash-programming (bootloader) mode, where the PDT ends at
`F34 + F01` and there is no `F12`. A reset cycle after `ENABLE_FLASH_PROG`
brings it back to application mode, and the Android driver then works normally
again - `input2` with `ABS_X` 0-1079 / `ABS_Y` 0-2159 / 10 `ABS_MT_SLOT`s and
real `ABS_MT_POSITION_X/Y` events in `getevent`. The firmware image itself was
never missing; `tools/extract-ts-firmware.py` pulls it out of the recovery
kernel (`out/ts-fw-whyred.img`, 128,512 B) and `tools/sweet-ts-reflash.sh`
drives the vendor `doreflash` sysfs, where `echo 2` (not `1`) is FORCE.
`NOTES.md` has the full sequence.

**Reading the result without a serial cable.** Every test image carries
`sweetlog=1`, so the 4 MiB console ring on SYSTEM keeps the probe lines even
though the panel stops drawing them. Boot the whyred recovery with
`fastboot boot` (never flash, never format), then run
`tools/sweet-touch-answer.sh`: it pulls the ring and prints only the
`WHYRED TOUCH` lines plus the boot landmarks. That is how the `ack 0x20`
above was read.

### Debugging tips

- Stack samples (`SMP name[pid] slide S mh M` then `SMP name tN pc .. bt ..`): symbolicate shared-cache addresses with `ipsw dyld a2s dyld_shared_cache_arm64 <addr - S>`.
- `SWEET TOUCH: ...` lines show each touch bring-up step (clocks, SE2 firmware check, chip id `54:52`).


## Repository layout

| Path | What |
| --- | --- |
| `shim/` | Bare-metal boot shim (C + asm), boot image v4, Gold hand-off, GIC setup, payload loader |
| `xnu/patches/` | XNU patch series (applied in order by CI): sweet's 0001–0079, santoni/lavender/whyred 0080+ |
| `.github/workflows/xnu.yml` | Kernel CI build (defaults to `WHYRED`) |
| `shim/board.h` | Per-board panel/SoC constants for whyred and lavender |
| `tools/mkadt.py` | Apple Device Tree for whyred/lavender (Gold cores, memory map, MMIO) and the iPhone X identity; `--board whyred` selects 1080x2160 |
| `tools/mkpayload.py` | Packs kernel + ADT + boot-args into the shim payload |
| `tools/mkiosroot.sh`, `tools/sweet-root-tweaks.py` | Build and adjust the HFS+ iOS root |
| `tools/sweet-crash.py`, `sweet-log.py` | Symbolicate crash reports, read the persistent log |
| `tools/extract-ts-firmware.py` | Pull the Synaptics DSX image out of any boot image (finds the `const unsigned char[]` in the kernel blob), validate the header, write `out/ts-fw-whyred.img` |
| `tools/sweet-ts-reflash.sh` | Push that image to the vendor driver over the `doreflash` sysfs (`imagesize` -> `data` -> `doreflash 2` = FORCE) |
| `tools/build-touch-diag.sh` | Pack the whyred touch-diag image from a CI kernel (muted console, ring on, `sweetouchi2c` + 30 s F12 poll, `rd=emmcroot0`); `SWEETI2C=0` packs the bit-bang-only probe (never maps the QUP window), `SWEETOUCHPOWER=1` adds the late-probe LDO11 toggle, and `OUT_IMAGE=` picks the output name |
| `tools/build-ring-image.sh` | Pack a log/ring image from a CI kernel with the three iOS-root flags (`emmcrw=1 cs_enforcement_disable=1 unentitled_ios_sim_launch=1`), muted console and `rd=emmcroot0`; `--power` adds the late LDO11 toggle, `--qup` adds `sweeti2c=1`, `--native` opts into the 1080x2160 canvas. It never arms `sweetouchreboot=1`, so a finished probe stays up |
| `docs/` | Live board, status, touch notes and the per-month engineering log |

Not in git: partition backups, live device trees, Apple firmware and root images, build trees.
