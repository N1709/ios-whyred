#pragma once
/*
 * Per-board constants. build.sh passes -DBOARD_<NAME> (default: sweet).
 *
 * BOARD_MPIDR(i): MPIDR (Aff1:Aff0) of physical core i, the value PSCI
 * CPU_ON expects. IMEM_RESTART_REASON / PSHOLD: 0 = not known for the
 * board yet; reset() then relies on PSCI SYSTEM_RESET alone.
 * BOARD_XNU_MPIDR: if defined, xnu is started on that core with PSCI
 * CPU_ON (x0 = boot_args) instead of on the core ABL entered us on.
 */

/* Qualcomm numbers the architected timer PPIs 1/2/3/0, not the usual 13/14/11/10 */
#define VTIMER_INTID		19
#define PTIMER_INTID		18
#define PSCI_CALL		"smc #0"	/* Qualcomm TZ */

#if defined(BOARD_QEMU)
/*
 * QEMU virt (tools/qemu-shim.py), to run the shim's own code without a
 * phone: ginkgo's splash address and size (plain RAM here), virt's GIC,
 * timer PPIs and HVC PSCI conduit.
 */
#define BOARD_NAME		"qemu"
#define GICD_BASE		0x08000000UL
#define GICR_BASE		0x080a0000UL
#define FB_BASE			0x5c000000UL
#define FB_W			1080
#define FB_H			2340
#define BOARD_MPIDR(i)		((uint64_t)(i))
/* ginkgo's xnu core; virt with GICv3 numbers vCPU 16 as Aff1=1 (-smp 17) */
#define BOARD_XNU_MPIDR		0x100UL
#define BOARD_HAS_UFS		0
#define IMEM_RESTART_REASON	0UL
#define PSHOLD			0UL
#undef VTIMER_INTID
#undef PTIMER_INTID
#undef PSCI_CALL
#define VTIMER_INTID		27
#define PTIMER_INTID		30
#define PSCI_CALL		"hvc #0"

#elif defined(BOARD_GINKGO)
/* Redmi Note 8, SM6125. Mainline sm6125.dtsi + sm6125-xiaomi-ginkgo-common.dtsi */
#define BOARD_NAME		"ginkgo"
#define GICD_BASE		0x0f200000UL
#define GICR_BASE		0x0f300000UL
#define FB_BASE			0x5c000000UL	/* reserved-memory cont_splash_mem */
#define FB_W			1080
#define FB_H			2340
#define BOARD_MPIDR(i)		((((uint64_t)(i) >> 2) << 8) | ((uint64_t)(i) & 3))
/* ABL enters on cpu0, a Cortex-A53: no 16K granule. The A73s have it. */
#define BOARD_XNU_MPIDR		0x100UL
#define BOARD_HAS_UFS		0		/* eMMC on sdhc_1 */
#define IMEM_RESTART_REASON	0UL		/* TODO: qcom,msm-imem restart_reason from the stock DTB */
#define PSHOLD			0UL		/* TODO: qcom,pshold from the stock DTB */

#else
/* Redmi Note 10 Pro, SM7150 (sweet live FDT) */
#define BOARD_SWEET		1
#define BOARD_NAME		"sweet"
#define GICD_BASE		0x17a00000UL
#define GICR_BASE		0x17a60000UL
#define FB_BASE			0x9c000000UL	/* reserved-memory cont_splash_region */
#define FB_W			1080
#define FB_H			2400
#define BOARD_MPIDR(i)		((uint64_t)(i) << 8)
#define BOARD_HAS_UFS		1
#define IMEM_RESTART_REASON	0x146aa65cUL	/* qcom,msm-imem restart_reason@65c */
#define PSHOLD			0x0c264000UL
#endif

#define FB_STRIDE		(FB_W * 4)	/* a8r8g8b8, matches mainline simplefb */
