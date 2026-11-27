# gpu

- **Hardware:** Adreno 509, `qcom,chipid = <0x05000900>`
  (`refs/whyred-android/dts_sdm636_whyred.dtsi`), GPU block `0x05000000`,
  GPUCC `0x05065000`, kgsl SMMU `0x05040000`, zap pas-id 13.
- **Code:** `whyred_gpu.c` is the boot-arg-gated bring-up ladder (patch
  `0211`, extended by `0217` and `0240`): GCC/GPUCC state, GDSC and RCG
  power-up, SCM zap query, ring, PM4/PFP microcode and fence.
  `WhyredGPU.cpp` is the IOKit service (`WhyredGPU`/`WhyredGPUUserClient`)
  with six selectors: info, contiguous buffer new/free, synchronous PM4
  submit, submit-by-buffer, and the Metal plug-in properties. `0240` adds
  the contiguous pool, the `CP_MEM_WRITE` service fence and the opt-in
  `whyredgpuclk=160|266` PLL/RCG switch.
- **State:** compiled and host-checked, **not booted on whyred**. The ladder
  is off unless `whyredgpu=N`; `30` starts the service, `31` also runs its
  self-test.
- **Metal:** the userland `WhyredMetal.bundle` is built from
  `lavender-metal` + hakios's Mesa patches on macOS; see `docs/metal.md`.
  It is not in this repository and needs the service verified first.
- **Next, in order:**
  1. Extract the Adreno firmware from the phone's own vendor partition and
     record the exact names and hashes in `hw.md`.
  2. Boot `whyredgpu=4` and confirm the GX GDSC and RBBM/CP registers.
  3. Boot `whyredgpu=12`, then `30`, and check the service appears and the
     self-test fence completes.
  4. Build/install `WhyredMetal.bundle`, then boot
     `whyredgpu=30 whyredmetal=1`.

Reference: `adreno/` (read `../THIRD_PARTY.md` first).
