# Roadmap


1. **Touch**: the controller answers at Synaptics `0x20` on BLSP1 QUP1 I2C
   (`0x0c175000`, IRQ GPIO 67, reset GPIO 66, VDDIO PM660 LDO11) and reports
   F12 again after the bootloader-mode recovery. 0184/0186/0187/0188 prove the
   bus and identify the chip with bit-banged GPIO; next is a clean read of the
   F12 data registers from XNU (the 0188 matrix exists to explain the shifting
   stream), then the QUP I2C master (GCC `BLSP1_AHB` + `QUP1_I2C_APPS`) and
   PM660 SPMI for LDO11 so the bit-bang can be retired. `sweet_hid_touch()` and
   `SweetHIDTouch` are already shared with the topaz port and need no change.
2. **Buttons**: power and volume keys on the PM660/PM660L PON block, volume up on a PMIC GPIO.
3. **Smoother display**: higher `sweetfb_hz`; check whether the Gold cores run at a low clock without cpufreq (the shim's speed probe) and raise it if so.
4. **Narrow the GIC fix** to the register that actually matters, then move it into XNU.
5. **Real-time clock** from the PMIC RTC or passed in by the shim.
6. **All 8 cores (experiment, later):** build XNU with 4K pages (drop `__ARM_16K_PG__` from `SWEET.h`, as XNU's own `BCM2837` Raspberry Pi 3 config does on Cortex-A53) and describe two clusters, Gold as `P` and Silver as `E`, so the AMP scheduler uses both. Risk: the iOS 16 userland was only ever run on 16K pages (hard-coded 16K in malloc, JIT guard pages, IOSurface, commpage layout), and the 16K assumptions in the shim and sweet's patches have to be checked too. The 16K Gold-only kernel stays as the fallback.
7. **Silver cores as a coprocessor** (idea): run bare-metal shim code on them for the panel frame copy instead of XNU.
8. **3/4/6 GB variants:** the ADT uses a fixed safe 1 GiB budget at `0xa0000000`, so the same image works on all whyred RAM variants. A larger budget needs the shim to read the DDR ranges from ABL's `/memory` node.
9. **Wi-Fi:** bring up the WCN3990 firmware through TrustZone/SMEM/QMI in an XNU driver. Expose it to iOS first as an IONetworkingFamily interface, then emulate the IO80211 interface that `wifid` talks to.
10. **Speaker:** ADSP/AudioReach through TrustZone, then an XNU audio driver that CoreAudio can output to.
11. **GPU:** the whyred Adreno 509 is close to the Adreno 610 work in [MakrSas/topaz-woa](https://github.com/MakrSas/topaz-woa) (`TopazGpuW`); a kernel msm/DRM-style user client is the long-term path for QuartzCore's OpenGL ES renderer.

