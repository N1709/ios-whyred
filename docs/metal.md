# Metal on whyred

The Metal path has three parts:

1. **Kernel service** (`drivers/gpu/`, patch `0240`): the Adreno 509 command
   processor, the `WhyredGPU` IOKit service, contiguous buffers and synchronous
   PM4 submission.
2. **Whyred userland layer** (`N1709/whyred-metal`): the A5xx register/PM4
   layer, the fake command processor and its host tests.
3. **Metal plug-in** (`WhyredMetal.bundle`): the `MTLDevice` implementation,
   the render/compute builders and the AIR -> NIR -> ir3 shader compiler. Its
   source is in `whyred-metal/userland/plugin/`, adapted from
   `aLpHa-Git-69/lavender-metal` and hakios's Mesa patches; it is not in this
   repository.

The kernel half is merged and host-checked. The plug-in source is in
`whyred-metal`; it still has to be built on a Mac and copied into the iOS
root. Nothing has run on whyred yet: the GPU ladder itself has never been
booted (`drivers/gpu/README.md`).

## Whyred GPU repository

[`N1709/whyred-metal`](https://github.com/N1709/whyred-metal) is the whyred
implementation: `userland/include/whyred-metal/` (A5xx registers, PM4 packets,
fake CP), `userland/tests/cp_test.cpp`, the hardware map in
`docs/HARDWARE-A509.md`, the plan and the ladder notes. Its `kernel/` copies
are snapshots of this patch series; the patches here are canonical.

`userland/plugin/` holds the Metal plug-in: `WhyredMetal.m`, the A5xx builders
(`why5.c`, `why5r.c`), the compiler wrapper (`whycc.cpp`) with hakios's `hk/`,
the Mesa register headers and `build-whyredmetal.sh`. It is renamed for this
service (`WhyredGPU`, `WhyredMetal.bundle`, `WhyredMetalDevice`) and the
compiler targets GPU id 509. It is **source only, not built yet**.

## Kernel side (0240)

The service now has six selectors:

| # | Selector | What |
| --- | --- | --- |
| 0 | `kWhyredInfo` | version, ready, submissions, last fence |
| 1 | `kWhyredBufNew` | contiguous buffer `{size}` -> `{handle, address}` |
| 2 | `kWhyredBufFree` | release a buffer |
| 3 | `kWhyredSubmit` | inline PM4 words -> fence |
| 4 | `kWhyredSubmitBuf` | PM4 words already in a client buffer -> fence |
| 5 | `kWhyredMetal` | set/remove `MetalPluginName` and `MetalPluginClassName` on `IOAcceleratorES` |

A buffer is a page slice of one contiguous pool (128 MB down to 4 MB, taken
when `whyredgpu=30` starts the service) or its own allocation when the pool is
gone. The pool matters because the Metal plug-in allocates many small buffers
after iOS has fragmented the low 4 GB.

The service submission uses `CP_MEM_WRITE` for the fence (a plain command
processor write, the same one the client's own streams use), spins 2 ms and
then sleeps 1 ms at a time up to 15 s. `whyredgpu=30` also switches the
bring-up announcements from 2 s to 10 ms, so the service is up well before
`backboardd` starts matching `IOAcceleratorES`.

### Boot arguments

| Argument | What |
| --- | --- |
| `whyredgpu=30` | run the ladder and start the IOKit service |
| `whyredmetal=1` | publish `WhyredMetal` / `WhyredMetalDevice` on `IOAcceleratorES` |
| `whyredgpuclk=160` or `266` | switch the GFX3D RCG/PLL from XO to the given frequency (off by default) |
| `whyredgpu_verbose=1` | log every service submission and buffer map |
| `whyredgpudelay=S` | wait before step 1 (default 25 s) |

Do not set `whyredmetal=1` until `WhyredMetal.bundle` is in the root: Metal
will then find the property and fail to load the missing bundle.

The clock values are the SDM660 GPUCC values; whyred uses the same
`qcom,gpucc-sdm660` block. Confirm the PLL lock at `whyredgpu=4` before using
`whyredgpuclk` on a boot that matters.

## Userland side

Sources:

- `https://github.com/N1709/whyred-metal`: the whyred A5xx layer, fake CP,
  host tests and the plug-in source in `userland/plugin/`. The plug-in belongs
  here, not in ios-whyred.
- `https://github.com/aLpHa-Git-69/lavender-metal` at `cc5b8db`
  (`userland/`, the plug-in `LavenderMetal.m` and its A5xx builders): the
  source the whyred plug-in was adapted from.
- `https://github.com/oosexgang/hakios-xnu-kernel` at `63fc2b4`,
  `components/metal/plugin/tools/mesa` (the AIR front end, ir3 patches and
  `hakios-drm`). MIT; see `userland/plugin/hk/NOTICE`.

The whyred plug-in is already renamed:

- service name `WhyredGPU`
- bundle `WhyredMetal.bundle`
- class `WhyredMetalDevice`
- GPU id `509` (`WHYRED_GPU_ID=509`)

### Build (macOS)

```sh
git clone https://github.com/N1709/whyred-metal.git
git clone --depth 1 --filter=blob:none --sparse \
    https://github.com/oosexgang/hakios-xnu-kernel.git hakios
git -C hakios sparse-checkout set --no-cone components/metal/plugin/tools/mesa

cd whyred-metal
B=/tmp/mi tools/mesa-ios/build-mac.sh ../hakios/components/metal/plugin/tools/mesa
cd userland/plugin
bash build-whyredmetal.sh
```

The Mesa build needs Xcode, the iPhoneOS SDK, `meson`, `ninja`, `mako`,
`pyyaml` and a newer bison; `build-mac.sh` installs what it can under `$B`.
The result is `userland/plugin/out/WhyredMetal.bundle` and
`out/whyredmetal_test`.

Copy it to `/System/Library/Extensions/WhyredMetal.bundle` in the iOS root
(the root-image builder, not a partition write) and boot with:

```
whyredgpu=30 whyredmetal=1
```

## Order of work

1. Extract the Adreno firmware from the phone's vendor partition and record
   the names/hashes (`drivers/gpu/hw.md`).
2. Boot `whyredgpu=4`; confirm the GX GDSC and the RBBM/CP reads.
3. Boot `whyredgpu=12`; confirm the ring, microcode and fence.
4. Boot `whyredgpu=30`; confirm `WhyredGPU` appears and the self-test fence
   completes (`whyredgpu=31`).
5. Install the bundle and boot with `whyredmetal=1`; check
   `MTLCreateSystemDefaultDevice()` and `/var/root/whyredmetal.log`.

Step 4 is the gate: a Metal plug-in on top of a command processor that has
never run cannot be debugged from the plug-in side.
