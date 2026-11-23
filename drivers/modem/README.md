# modem

- **Hardware:** SDM636 MSS/QDSP6, firmware in the GPT `modem` partition.
- **Code:** `whyred_modem.c` (0215) contains the opt-in MSS clock/reset, RMB,
  MBA/MDT loading and TrustZone PAS ladder. It obtains the partition LBA from
  the GPT rather than using another board's fixed offset.
- **State:** compiled and host-checked, off unless `whyredmodem=N`, and never
  run on whyred hardware. It is a bring-up ladder, not a cellular service.
- **Missing:** a proven firmware boot, shared-memory/QMI channels, SIM/radio
  control, data networking, telephony integration and safe suspend/resume.
- **Order:** test read-only GPT/FAT discovery first, then one clock/reset rung
  per boot. Do not enable cellular by default until every PAS stage is proven.
