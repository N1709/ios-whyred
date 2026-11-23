# audio

- **Hardware:** Qualcomm WCD audio codec on SLIMbus (the exact part must be
  read from the full whyred DTS; the mirrored board files only carry the
  `btfmslim_codec` node) and the ADSP.
- **Code:** `whyred_audio.c` (0213) contains the boot-arg-gated LPASS/ADSP
  power, SMMU, PAS and modem-FAT firmware ladder. It is compiled but off
  unless `whyredaud=N`; no step has run on whyred hardware.
- **Missing:** the WCD9335/SLIMbus codec path and the `IOAudioFamily` service
  CoreAudio needs. An authenticated ADSP alone does not produce sound.
- **Next:** confirm the codec part and its SLIMbus master from the Android DTS,
  then decide whether a codec-only path (no ADSP) is enough for boot sounds;
  SpringBoard does not require audio to render.

Reference: `dsp/` (read `../THIRD_PARTY.md` first). It targets the same
SDM636 ADSP over GLINK/SMEM, but does not carry a codec path yet. The codec
registers and firmware still have to be confirmed from the Android DTS.
