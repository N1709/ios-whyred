# Roadmap


1. **Visible lock screen:** verify 0229 makes IOMFB report P3 D65 instead of
   sRGB and removes the `BoundingPathBitmap` SpringBoard crash. Do not stack
   another display variable into that boot.
2. **Normal touch:** the DSX at `0x20` still streams after the current short
   LDO11 cycle. In separate diagnostics, prove a 200 ms off/readback/on cycle,
   then general-call reset and QUP framing. Only an addressable part gets F01
   normal mode, F12 `ctrl20`/`ctrl28`, the ten-finger poll and
   `SweetHIDTouch` tap/swipe/multi-touch.
3. **Adreno 509:** run the existing A5xx ladder one rung per boot, starting
   read-only. Prove clocks/GDSCs, chip identity, firmware, ring and fence
   before exposing an accelerator; Metal must use the real GPU path.
4. **Buttons, RTC and storage DMA:** code exists (0208-0210); verify the
   already-built paths from the ring rather than adding replacements.
5. **Power:** replace the static battery with PM660 charger/fuel-gauge data
   only after register ownership is confirmed. Keep unverified PMIC writes
   disabled.
6. **Networking:** `whyred_wlan.c` is only a WCN3990 platform/SMMU probe and
   `whyred_modem.c` only an MSS/PAS ladder. Add firmware/QMI, data transport
   and iOS networking services after read-only hardware rungs pass.
7. **Audio:** prove the ADSP ladder, then add the missing WCD9335/SLIMbus and
   CoreAudio service; ADSP authentication alone is not sound output.
8. **Bluetooth:** separate WCN3990/SLIMbus work after Wi-Fi and audio clocks
   are understood.
9. **All 8 cores (later experiment):** a 4K-page two-cluster kernel risks
   iOS 16's 16K assumptions. Keep the proven Gold-only 16K kernel as the
   fallback.
10. **3/4/6 GB variants:** the fixed safe 1 GiB ADT budget works across them;
    any larger budget must come from ABL's actual DDR ranges.
