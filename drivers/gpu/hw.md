# gpu hardware notes

Facts taken from `refs/whyred-android/dts_sdm636_whyred.dtsi` and
`dts_sdm660_whyred.dtsi`, plus the A509 entry in Linux `a5xx_catalog.c`.

| Item | Value |
| --- | --- |
| GPU | Adreno 509 |
| Chip ID | `0x05000900` (`&msm_gpu { qcom,chipid = <0x05000900>; }`) |
| GPU block | `0x05000000`, 256 KiB |
| GPU clock controller | `clock-controller@5065000`, `compatible = "qcom,gpu-sdm660"` |
| GPU CC | `0x05065000`, `compatible = "qcom,gpucc-sdm660"` |
| kgsl SMMU | `0x05040000`, IRQ SPI 300 |
| Zap | `a512_zap.mdt` through SCM pas-id 13 (the A509 uses the A512 ZAP) |
| CP microcode | `a530_pm4.fw`, `a530_pfp.fw` |
| GPMU | none |
| GMEM | 256 KiB + 16 KiB |
| Panel | 1080x2160, stride 4320 |

Open questions before the first device run:

- exact firmware file names and where the vendor partition stores them;
- the `qcom,gpu-pwrlevels-*` frequency/corner ladder in the whyred DTS;
- whether the bootloader leaves the GPU rail high enough for `whyredgpu=4`.
