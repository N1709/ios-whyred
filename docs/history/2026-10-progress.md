# README progress narrative (moved 2026-11-17)

This was the `## Progress` section of the README. It stops at
2026-10-31; later entries are in `2026-11.md`. Kept verbatim.
## Progress

**2026-10-31 - a clock that looks on is not a live slave; verify the NOC FSM.**
The 41b3069 boot reached fastboot by itself once (132 s after the diag
image started) but a later boot of the same image stalled at the colour bars
again, so the QUP access is not deterministic: when the BLSP1 clocks really
come up the probe completes, and when they do not the first SE register read
never returns and the CPU stops with it.

`clk-branch.c` accepts a branch whose `BIT(31)` off flag is clear - it does
not require the NOC FSM to report `on` (`0x2 << 28`). That is too weak here:
the branch can read "not off" while its slave still has no response. The
driver now requires `(halt & BRANCH_NOC_MASK) == BRANCH_NOC_ON` for both
BLSP1 AHB and the QUP1 apps branch, prints the raw CBCR/RCG values when the
check fails, and returns to the bit-bang without touching the QUP at all. A
bad clock now costs a diagnostic line, not a stalled phone.

**2026-10-31 - CI moved to `macos-15`, completed.** Three runs in a row
(`8c3f96e`, `7e2bf21` twice) were cancelled one second after the job started
with "scheduled macOS 14 runner image brownout". `.github/workflows/xnu.yml`
now uses `macos-15`; the Deps step already picked the *lowest* Xcode on the
image, which is 15.x there, so `iig` keeps the IORPC ABI xnu-8796 expects. The
first run after the switch (`41212963199`, `be727db`) succeeded and produced
the kernel used for ring 0195 - this item is finished, no further action.

**2026-10-31 - the diagnostic boot returns to fastboot on its own.**
A diag boot ends with the phone running XNU, which has no USB driver on this
port: the only way back was a physical Power + Vol-Down. `0190` now sets
`sweetouchreboot=1` in the diagnostic cmdline, and after the probe the kernel
does what the vendor `msm-poweroff.c` does for `reboot bootloader`:

- write `0x77665500` (`RESTART_REASON_BOOTLOADER`) to IMEM
  `0x146bf65c` - `sdm660.dtsi` names `qcom,msm-imem@146bf000` with
  `restart_reason@65c`;
- drop PS_HOLD by writing 0 to `0x010ac000` (`restart@10ac000`,
  `compatible = "qcom,pshold"`).

IMEM is always-on and survives the warm reset, so ABL sees the reason and
enters fastboot. The probe waits 3 s (two log-flusher ticks) before the reset,
so the ring is on SYSTEM first. `tools/touch-cycle.sh` now runs end to end
without a key press: bootloader, diag image, dwel, fastboot, recovery,
`fastboot boot`, ring pull.

Note the timings first: a full diag boot is ~5 minutes (bit-bang scan, framing
matrix, DSX dump, 15 s panel hold), so `tools/touch-cycle.sh` waits up to 420 s
for the return. A phone that looks hung after 4 minutes is usually still
working.

That reset is only reached if the probe finishes. A stall in the GCC or QUP
register window happens earlier, and the next diag boot proved the cost: the
phone sat on the colour bars with no USB device at all until Power was held
for 15 s. `0190` therefore arms a reboot watchdog on its own kernel thread
*before* the first clock write (`sweetouchwatchdog=<seconds>`, default 240,
0 disables). The timer runs on another core; the IMEM reason and PS_HOLD are
outside the BLSP window that stalls, so even a hung probe lands in fastboot.

**2026-10-31 - the GCC block starts at `0x00100000`, not `0x01800000`.**
Two boots in a row ended at `WHYRED TOUCH: ack 0x20` with a byte-identical
ring. The scan itself is fine (0189 runs to `scan done` with the same bus), so
the stop is the first QUP register access: it is reached less than a second
after that line, before the flusher's next tick can commit anything newer.

The vendor DTS names the block: `clock-controller@100000` with
`reg = <0x100000 0x94000>`. The offsets in `gcc-sdm660.c` (`0x52004`,
`0x17004`, `0x19008`, `0x19020`) therefore land at `0x152004`, `0x117004`,
`0x119008` and `0x119020`. Addressing them from `0x01800000` - the base the
SDHCI code still uses - writes to `0x1852004`, above the block, so the BLSP1
AHB and QUP1 apps branches were never really enabled. The QUP then has no
clock, its register window does not answer, and the access stalls the AXI
bus. The same bug hides in `GCC_SDCC1_APPS_PHYS 0x01842000`, which is why the
SDHCI rate change never took effect.

`tools/touch-cycle.sh` now runs a whole cycle without typing: bootloader,
diagnostic image, dwell, `fastboot boot` of the OrangeFox recovery (this ABL
does not honour `fastboot reboot recovery`), ring pull and grep. The bring-up
line and a 1.5 s dwell also now come *before* the first GCC/QUP access, so if
a block still stalls, the ring names the block that was touched last instead
of stopping one line earlier.

**2026-10-31 - the first QUP boot stopped at the probe; three addresses were wrong.**
The 0190 image painted the four colour bars (0176) and then stopped: the ring
ends at `WHYRED TOUCH: ack 0x20`, before `sweet_i2c_init()` could print its
first line. The address scan itself was fine; the hang was the first QUP
access, which went to `0x00052004` instead of `0x01852004` because the vendor
offsets had been used as absolute addresses. The same mistake put the I2C RCG
at `0x00019024` (should be `0x01819024`), and GPIO 2/3 were mapped at
`0x03002000` instead of the TLMM SOUTH tile `0x03102000`.

Reading the vendor driver again also caught a count error that would have
killed the very first write-then-read: `MX_WRITE_COUNT` includes the four tag
bytes of the *read* buffer as well, so it is `(wl + 4) + (rl ? 4 : 0)`, not
`wl + 4`. The pin mux now happens as the last step of init, so a failure
leaves the pads as plain GPIO and the bit-bang fallback still works.

**2026-10-31 - 0189 says the bit-bang cannot address the chip; 0190 moves the reads to the QUP.**
All seven framings 0189 tried came back `streams`: `0x00e9` returned
`12 0a 29 00 8f d4`, then `81 04 03 03 04 05`, then `05 01 01 01 0e 0f` - six
new bytes every time, no matter what address or how many address bytes were
sent. The bit-bang is therefore not fixable by timing: the controller never
sees an address phase at all.

`0190` adds `bsd/dev/sweet_i2c.c`, a BLSP1 QUP1 master, and points the DSX
reads at it behind `sweeti2c=1` (the bit-bang stays as the fallback, including
the address scan, which still finds `0x20`). The register set, clock tree, pin
mux and FIFO protocol were taken from the vendor `i2c-msm-v2.c` rather than
guessed, which corrected four things that would each have wasted a boot:

- `QUP_I2C_MASTER_CONFIG` (0x408) must be set to 1 for the v2 tags; without it
  the SE does not treat `0x8x` bytes as tags.
- a tag is `[START, addr<<1|r/w, tag, len]`, little endian in the FIFO word,
  and the stop bit is the `+1` variant (`0x83`/`0x87`), not the `0x82`/`0x85`
  one - 0189's driver had the two swapped.
- `MX_WRITE_COUNT` counts data + 4 tag bytes and `MX_READ_COUNT` counts data
  + 2 (the input FIFO echoes two tag bytes first), all in FIFO mode with
  `MX_INPUT/OUTPUT_COUNT` zero.
- the RCG is at `0x19024` with the divider in bits 0-4 and the parent in bits
  8-10; the branches enable with bit 17 (`BLSP1_AHB`, 0x52004) and bit 0
  (`I2C_APPS`, 0x19008) and report halt in bit 31.

The driver fills the FIFO in PAUSE, moves to RUN and polls `MAX_INPUT_DONE`,
so a wrong count prints `timeout OPER/ERR/STATUS` instead of hanging. The
per-boot table now also names `QUP1 (hardware I2C)` or `bit-bang` before the
framing matrix, so one ring shows which transport produced the bytes.

**2026-10-30 - the controller was in bootloader mode; 0189 tests the address framing.**
The touch controller had been parked in flash-programming mode, not stripped of
firmware: the page table was `F34 + F01` with no `F12`. A reset cycle after
`ENABLE_FLASH_PROG` boots it back into application firmware, and the Android
recovery driver then works normally again - `input2` with 10 `ABS_MT_SLOT`s and
real `ABS_MT_POSITION_X/Y` events in `getevent`, cleared by a boot of
`fastboot boot` recovery. The firmware was never missing either:
`tools/extract-ts-firmware.py` pulls the 128,512-byte DSX image out of the
recovery kernel, and `tools/sweet-ts-reflash.sh` drives the vendor
`doreflash` sysfs (`echo 2`, not `1`, is FORCE).

XNU still reads the same shifted stream, and 0188's matrix pinned it down: a
read of `0x0000` returns the entry that lives at `0x00e9`
(`27 00 09 00 21 34`), and a second read of the same address returns the next
six bytes, so the address phase never lands and the chip does no more than
stream from wherever its pointer sits. The created tooling is in `NOTES.md`.
`0189` therefore enumerates the framings nobody has tried - 16-bit address
little endian, the page select and the address in one write, the page select
with a two-byte address - and accepts a framing only when it returns the
`0x00e9` entry twice in a row and something else one slot above; the winner is
used for the rest of the probe. It also recognises the `F34 + F01, no F12`
table as bootloader mode and pulses reset once before giving up.

**2026-10-29 - 0187 and 0188: the read is the problem, not the address.**
`0187` replaced the RMI4 read with the Synaptics DSX one the vendor driver
uses (page select `0xFF <page>`, then one address byte, then a repeated
start). The result was still wrong, and the way it is wrong is the useful
part: reading `0x00e9` returns `27 00 09 00 21 34` and then `0x00e3`
continues the same stream with `2f 26 0d 04 01 01` instead of starting a new
page-table entry, and the first slot decodes as function `0x34` - the slot at
`0x00e9` always holds F01. So the address phase is not taking effect at all.
`0188` stops guessing and answers four questions in one boot, read-only:
does reading the same address twice repeat or advance; does a two-byte address
(the 0186 protocol) behave differently; does `0x00e8`/`0x00ea` shift the
answer; and does `0xFF <page>` reach page 1? It also makes the bit-bang
open-drain (release high, wait for the line) so a controller holding SCL low
while it fetches a register is no longer overridden, drives reset with the
timings the DT asks for (20 ms low, 200 ms high), and watches IRQ GPIO 67
during the touch poll: a level change proves the controller is scanning even
if I2C reads stay broken.

**2026-10-29 - whyred's touch controller answers.** Patch `0184` bit-bangs the
BLSP1 I2C bus as plain TLMM GPIO (SDA 2 / SCL 3) and scans every address. The
SYSTEM ring, pulled after a `fastboot boot` of the whyred recovery with
`tools/sweet-touch-answer.sh`, has the answer:

```
WHYRED TOUCH: rst cfg 000003c3 io 00000003; irq cfg 00000001 io 00000001
WHYRED TOUCH: sda2 ctl 000000c3 scl3 ctl 000000c3; idle sda 1 scl 1
WHYRED TOUCH: ack 0x20
WHYRED TOUCH: scan done, 1 address(es) acknowledged; sda 1 scl 1
```

So the controller is powered, out of reset (reset pad is an output driven
high, IRQ is an input idling high) and answering; the "no power / held in
reset" branch is dead. `0x20` is the Synaptics address, so **this unit is not
the Novatek `0x62`/`0x01` variant**. `0185` explains why none of this was
ever visible: the console stops being drawn at the framebuffer hand-off, so
the result is now painted into the scanout buffer instead of printed. `0186`
adds a read-only RMI4 identification pass (version register, function
descriptor table, F01 and F12 query blocks) to learn the product string and
the F12 control/data addresses before anything enables reporting.

Last updated 2026-10-29. **The iOS lock screen is on the panel.** With the
`0182` kernel the phone boots past the data-migration spinner into
SpringBoard's lock screen: the Apple mark holds through the kernel boot, then
the first iOS frame replaces it and the UI stays up (`e77`, first boot of
`0180`+`0181`+`0182`). That closes the original goal of this port - the iOS UI
is visible on whyred's own panel, from RAM, without touching a boot partition.

What the lock screen does **not** have is input. On whyred the HID services are
registered (`SWEET HID: touch registered`, `backboardd` opens
`SweetHIDTouch`) but nothing feeds them: `bsd/dev/sweet_touch.c` and
`bsd/dev/sweet_i2c1.c` are compiled only for `ARM64_BOARD_CONFIG_SWEET`
(topaz), so on whyred `SweetHIDTouch` is an empty digitizer. The facts a
driver needs, recovered from the whyred vendor DTS: touch sits on BLSP1 QUP1
I2C (`0x0c175000`, SDM636), Novatek `0x62` (IRQ GPIO 67, reset GPIO 66,
VDDIO PM660 LDO11) or Synaptics DSX `0x20` depending on the panel variant
(tianma/huaxing vs shenchao). None of the buses that address it exist in this
kernel yet - no TLMM GPIO, no BLSP I2C, no PM660 SPMI on the whyred side.

There is also no GPU driver: `backboardd` composites in software into the
`IOSurface` that `AppleCLCD` scans out, so the UI is correct but slow.
That is a separate project from the one this README tracks (panel + kernel).

`0179` boots the same userspace as `0178`, and the
`SWEET` ring finally shows the presentation path end to end:
`exec backboardd` → `exec SpringBoard` → `backboardd` creates IOSurfaces →
`SWEET FB: display on` → `swap 3/4/7 layer 0 surface 3 1080x2160` → the
lock-screen daemons (`donotdisturbd`, `chronod`, `passd`, `liveactivitiesd`)
start. So SpringBoard is neither dead nor looping; it runs and spawns its
lock-screen services.

The ADT canvas is now the panel: `tools/mkadt.py` published the iPhone X
canvas (1125x2436) unless `--native` is passed, and `shim/build.sh` called it
without that flag. The blob on the phone now carries 1080x2160 for both
`canvas-*` and the `sweet-fbcon` node (verified byte-wise: the old blob holds
1125/2436, the new one only 1080/2160). Hardware shows the limit of that fix:
`backboardd` still creates `id 2 1125x2436` next to `id 3 1080x2160`. Those
1125 surfaces come from userland (SpringBoard lays out for the iPhone X it
believes it is running on), not from the ADT; the surface actually presented
is `id 3`, 1080x2160. So geometry is no longer the blocker.

What remains is that the new image presents only **three frames**
(`swap 3/4/7`) and then stops. The panel shows the data-migration spinner and
nothing moves. Two measurements must not be confused with that state: the
later ring (`/tmp/whyred-e76.txt`) was pulled from an **older hand-built
image** - its `boot-args` line has `cs_debug=2 -enable_kprintf_spam` and no
`sweetfb_stayon` / `sweetfb_noconsole` / `sweetfb_hz` - so its console text
over the GUI and its `display off` belong to that image. The new image's ring
(`/tmp/whyred-e75.txt`) says `display off ignored (sweetfb_stayon)` and its
console is muted, as intended.

`panel_console=hidden` on a manual run (or `PANEL_NOCONSOLE=1` by hand) adds
`sweetfb_noconsole=1`, which stops drawing the kernel console over the GUI on
the first swap while the SYSTEM ring keeps the text. `disable_kprintf_output`
was assumed to do this; it does nothing here, because this kernel has no
boot-arg by that name - only the symbol. The Apple logo from lavender
(`92f8e72`) is ported with whyred's own Y computed from the panel height, not
lavender's hard-coded 800. The shim's mark only survives the blink between
the jump and the XNU fbcon clear, so `0182` paints the same 208x256 bitmap
again in `AppleCLCD::start()` and mutes the console there when
`sweetfb_noconsole=1`: the panel then shows the mark for the whole kernel
boot, until the first iOS frame replaces it.

`0179`'s local panic was not the kernel: the hand-built images carried
`ignition_level=1 rootrw=1 wdt_*`, which makes libignition search a preboot
volume that does not exist (`ignition boot failed`) and pid 1 then dies with no
dyld cache. CI never had those switches; `payload_cmdline.py` now rejects
`ignition_level=` on every image so a hand build cannot reintroduce it.

Two kernel defects kept that state unmeasurable. `0180` answers
`DKIOCSYNCHRONIZE` on the SDHCI root: HFS calls it before advancing the journal
head, the driver fell through to `return ENOTTY`, and every checkpoint printed
`jnl: b(2, 0): flushing fs disk buffer returned 0x19` (0x19 is `ENOTTY`). The
card cache is never enabled, so returning success is honest. `0181` makes the
console flusher (`0143`) retry a busy card instead of stopping: `fsck` reads
the same card for minutes, a `CMD24` into that traffic times out, and the old
code returned, leaving the tail of the boot - the only place the late `exec`
lines live - out of the ring for good. The next boot with `sweetlog=1` can now
say whether `backboardd` really stops after three frames or whether the log
was simply truncated.

`0177` is the previous hardware-confirmed display state: it draws four colour
bars (red/green/blue/white) into the ABL framebuffer
at `0x9d400000` during the SANTONI probe, before `launchd`, and they appear
on the panel. That settles the display question — MDSS/DSI keep scanning the
boot framebuffer out after hand-off, so the black boot screen is the UART
console plus `kPEAcquireScreen` clearing the buffer, not missing display
hardware. It also proves the `0177` mapping fix: `0176` panicked with
`KERN_NO_SPACE` because `ml_io_map_wcomb()` allocates from the 8 MiB
`io_submap`, and the bars need 9,331,200 B.

The code-signing gate was cleared by `0174`: `SpringBoard` execs, opens
`IOPMrootDomain` and its `SWEET AKS` calls return `0x0`, with no
`csblob invalid` line. That closed the `csblob invalid: 85` /
`vm_shared_region_slide_page()` failures traced in `0173b`: the driver
returned bytes produced by a *different* request, because it copied the
shared bounce buffer after dropping its lock. `0174` keeps the copy inside
the transfer's critical section.

One thing still separated that daemon set from a usable screen: nothing
presented into the framebuffer yet. `0179` closed that - `backboardd`
presents 1080x2160 frames - and the remaining gap is the canvas geometry
described at the top of this section. Separately,
`installcoordinationd` failed with `NSPOSIXErrorDomain 13` creating its data
directory, the same EACCES that SWEET fixed by dropping `config_ext_resolver`
(`0057`); `0175` does that for WHYRED/LAVENDER. The boot-stage checklist
stays in the chat, not here.

`0178` repairs the SYSTEM console ring that 0177 exposed: the log boot wrote
26 blocks of real console text but never a header, so every `sweet-log-pull`
refused the area and the log looked empty. The header is now written before
the first ring block and a header failure no longer stops the flusher; the
pull tool can also decode a headerless ring.

The 0177 UART capture settles the daemon question: `SpringBoard` execs, opens
`AppleKeyStore` and `IOPMrootDomain` (`0x0`) and its AKS calls succeed, so
the black screen is not a dead SpringBoard. `0179` then showed
`backboardd` creating surfaces and presenting them, which moved the missing
piece from "presentation" to "the canvas the frames are drawn for".

| Milestone | Progress | Notes |
| --- | --- | --- |
| Kernel boots, console, framebuffer, userspace `init` as PID 1 | **100%** | Gold cores, GICv3, timers, `AppleCLCD` at 1080x2160, ramdisk init runs as PID 1 and ticks every 5 s. Proven on hardware since `0172`. `0177` adds the missing proof that the panel scans this framebuffer out after hand-off: four bars written by the kernel are visible. The boot screen is still black by design because the console is routed to the UART and `kPEAcquireScreen` clears the buffer |
| eMMC command, data and mount path | **100%** | 8-bit HS, EXT_CSD, GPT, 58.2 GiB, single- and multi-block transfers, journal replay, HFSX mounted read-write. The last defect was the bounce buffer being copied outside its lock (the source of the wrong code-signing bytes); `0174` owns every byte while the transfer holds the lock |
| Root filesystem from eMMC (`launchd`, real daemons) | **100%** | `launchd` runs as PID 1; `bindfs` lets the whole boot-task chain (`mount-phase-1`, `mount-phase-2`, `fips`, ...) exit successfully, and the LaunchDaemons start (`watchdogd`, `lockdownd`, `backboardd`, `amfid`, `imagent`, ...) |
| Code signing / AMFI (trust cache, entitlements) | **90%** | Confirmed on hardware: `SpringBoard` (a platform binary) execs, queries IOPMrootDomain and its `SWEET AKS` calls return `0x0`, with no `csblob invalid`. `0179` adds the same proof for `backboardd` in the full `sweetlog` ring (`exec backboardd`, IOSurface create/present). Remaining for 100%: removing the `unentitled_ios_sim_launch` / `launchd_unsecure_cache` bring-up switches |
| Daily-driver usable (touch, Wi-Fi, sound, battery) | **15%** | NT36672 touch, WCN3990 Wi-Fi and ADSP audio are still untouched. The touch work since 0190 deliberately does not move this number: the GCC/clock path and the reboot watchdog are proven (`clocks on, RCG cfg 0x1 cmd 0x2, apps 0x1`), but the first SE access at `0x0c175000` stalls, so not one touch byte has been read through the QUP - zero functional events. Ring 0195 (build fc42b0ea) proves the clock side end to end (`clocks on, RCG cfg 0x1 cmd 0x2, apps 0x1`) but the first SE access at `0x0c175000` stalls, so no touch byte has been read yet and the row counts only what a user can do |
| **Kernel readiness for an eMMC boot** | **100%** | Mount, bind mounts, platform-binary exec, the full boot-task chain and `SpringBoard` are proven on hardware by `0174`; `0175` only removes a permission bug on the userspace side |
| **Overall port (weighted)** | **~90%** | The kernel and the panel scan-out are done; what is left is userspace presentation (`backboardd`/`SpringBoard` opening `AppleCLCD`) and the peripherals; the CI runner move to `macos-15` is finished (run 41212963199 succeeded), so what is left is userspace presentation plus the BLSP1 QUP1 SE, which stalls on its first register write and is the only thing between this port and a touchable device |

The percentages describe this port, not the upstream `ios-sweet` project. They
move only when a change is confirmed on the phone, not when a patch compiles.

