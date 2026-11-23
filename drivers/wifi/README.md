# wifi

- **Hardware:** Qualcomm WCN3990 (`compatible = "qca,wcn3990"` in
  `refs/whyred-android/dts_sdm660_whyred.dtsi`), Bluetooth on the SLIMbus
  (`btfmslim_codec`).
- **Code:** `whyred_wlan.c` (0214) is a read-only, boot-arg-gated platform
  probe for WCN3990 and the ANOC2 SMMU. It is compiled and off unless
  `whyredwlan=1`; it has not run on whyred hardware.
- **Missing:** firmware/QMI start, SNOC data transport, 802.11 MAC and an
  `IO80211`/IONetworkingFamily service. The probe is not usable Wi-Fi.
- **Notes for the first attempt:**
  - The Android driver is `ath10k_snoc`; the firmware files it loads on this
    phone must be copied out of the vendor partition and named here.
  - Bluetooth is a separate SLIMbus client and can be brought up later; it is
    not needed for the iOS boot.
  - Check the regulator and clock list in the DTS before writing attach code:
    the WCN3990 needs its `vdd*` rails up before the first QMI handshake.

Mirrors: `whyred_wlan.c` and `TopazModem.cpp`. The latter's WLAN stack is for
different hardware and is only an architecture reference for the user-client
shape.

Reference: none yet. WCN3990 uses `ath10k_snoc`; implement it from the Linux
driver only after the modem and its `vdd*` rails are proven on this board.
