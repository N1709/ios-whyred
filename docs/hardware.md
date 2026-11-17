# Hardware

Reference for the Redmi Note 5 / Note 5 Pro (`whyred`, SDM636).

## Hardware architecture & differences

| Component | Redmi Note 5 / Note 5 Pro (whyred / SDM636) |
| --- | --- |
| CPU | Octa-core Kryo 260 (4x Cortex-A73 Gold @ 1.8 GHz + 4x Cortex-A53 Silver @ 1.6 GHz) |
| 16K Granule | Supported on Gold cluster (`0x100`–`0x103`); Silver cluster is parked via PSCI `CPU_ON` |
| Interrupt Controller | ARM GICv3: GICD `0x17a00000`, GICR `0x17b00000` |
| Timers | VTIMER INTID 19 (GIC_PPI 3), PTIMER INTID 18 (GIC_PPI 2), 19.2 MHz |
| Storage | eMMC 5.1 (`sdhc_1` at `0x0c0c4000`, Qualcomm SDCC v5) |
| Framebuffer | `0x9d400000`, 1080x2160, 32-bit `a8r8g8b8`, stride 4320 bytes |
| Power / Reset | PS_HOLD `0x010ac000`, IMEM restart reason `0x146bf65c` |
| Touch | Novatek NT36672 at `0x62` (or Synaptics DSX at `0x20` on the shenchao panel) on BLSP1 QUP1 I2C `0x0c175000`; IRQ GPIO 67, reset GPIO 66, VDDIO PM660 LDO11. **Not enabled**: whyred has no TLMM/GPIO, BLSP I2C or PM660 SPMI driver in this kernel yet |


## Key hardware facts (whyred)

- SoC SDM636: 4× Kryo 260 Silver (A53 class, MPIDR `0x0`–`0x3`, 1.6 GHz) + 4× Kryo 260 Gold (A73 class, `0x100`–`0x103`, 1.8 GHz). Only Gold has the 16K granule.
- GICv3 at `0x17a00000` (redistributors `0x17b00000`, 8 PEs, stride `0x20000`), virtual timer INTID 19, physical timer INTID 18, 19.2 MHz.
- eMMC 5.1 SDCC v5 at `0x0c0c4000`, GIC SPI 110.
- Panel: 1080×2160 DSI video mode, continuous splash at `0x9d400000` (`cont_splash_mem` in the vendor DTS), 32-bit `a8r8g8b8`, stride 4320.
- Touch: **Synaptics at I2C `0x20`** on this unit (bus verified by 0184: `ack 0x20`, `idle sda 1 scl 1`, reset released). Novatek NT36672 at `0x62`/`0x01` is the other panel variant. No report path into HID yet.
- Vendor DTS source: [LineageOS/android_kernel_xiaomi_sdm660](https://github.com/LineageOS/android_kernel_xiaomi_sdm660), `arch/arm64/boot/dts/vendor/qcom/xiaomi/longcheer/whyred/`.

