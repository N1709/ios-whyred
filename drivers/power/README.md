# power

- **Hardware:** PM660/PM660L PMIC pair (charger, fuel gauge, regulators), plus
  PS_HOLD at `0x010ac000` which the shim already uses for reset.
- **Code:** `WhyredBattery.cpp` and `whyred_power.c` are compiled mirrors of
  the inherited battery/power path. The boot command line sets
  `sweetbattery=0`, so it publishes only the static fallback and does not
  touch an unverified PM660 gauge or charger.
- **Missing:** verified PM660 charger/fuel-gauge registers, thermal
  mitigation and real battery reporting to `IOPMrootDomain`.
- **Next:** not on the critical path for the lock screen; pick this up after
  SpringBoard, when the device must stay alive on battery.

Mirrors: `WhyredBattery.cpp` and `whyred_power.c`, renamed from the
sweet/topaz power path so the whyred tree carries no `sweet_*` power names.
The code is still the sweet/topaz charger/gauge, compiled through the shared
`ARM64_BOARD_CONFIG_SWEET` define and disabled by `sweetbattery=0`; the PM660
register programming still has to come from the whyred Android sources.

Reference: `pmic/`, `pmgr/` and `cpu/` (read `../THIRD_PARTY.md` first). They
target the same PM660/PM660L pair and clock tree. `pmic/` carries the rail
table and the `IOPMPowerSource` shape; `pmgr/` the clock/reset scheduling;
`cpu/` the CPU/GIC/timer override. Charge detection and the fuel-gauge
registers are still missing from both trees.
