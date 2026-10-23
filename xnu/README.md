# XNU (sweet / emerald)

This directory does **not** vendor Apple XNU sources. CI fetches them at build time.

## Licenses

| Component | License |
|-----------|---------|
| Apple `xnu` (and related OSS projects pulled by the build) | **APSL 2.0** (Apple Public Source License) |
| [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build) helper (`build.sh`) | **MIT** (pinned `DXB_SHA` in `.github/workflows/xnu.yml`) |
| Patches under `xnu/patches/` | Project patches applied onto APSL xnu |

## Supported ARM64 `MACHINE_CONFIG` values (after patches)

Upstream xnu (via build helper) + our patches:

| Config | Status | Notes |
|--------|--------|-------|
| `VMAPPLE` | upstream | default in blacktop `build.sh` |
| `BCM2837` `T6000` `T8101` `T8103` | upstream | Apple / Pi targets |
| `SWEET` | patch `0004` | Redmi Note 10 Pro (SM7150) |
| `EMERALD` | patch `0082` | Redmi Note 13 Pro 4G (MT6789); reuses SWEET paths, timer INTID 27 |

Expected CI kernel filename: `kernel.<kernel_config_lc>.<machine_config_lc>`  
Example: `KERNEL_CONFIG=DEVELOPMENT MACHINE_CONFIG=EMERALD` → **`kernel.development.emerald`**

`shim/out/Image` is a freestanding ARM64 **loader**, not an XNU kernel. Do not rename or treat it as XNU.

## Build (macOS GitHub Actions only)

```sh
gh workflow run xnu.yml -R dipa-studio/ios-sweet --ref emerald \
  -f macos_version=13.5 -f kernel_config=DEVELOPMENT -f machine_config=EMERALD
gh run watch -R dipa-studio/ios-sweet
gh run download -R dipa-studio/ios-sweet -n xnu-kernel-EMERALD -D xnu/out
python3 tools/verify_xnu_kernel_artifact.py xnu/out/kernel.development.emerald \
  --expect-name kernel.development.emerald
```

`PHYS_BASE` is unrelated to this compile and remains blocked until SSMR CMA runtime base is evidenced.
