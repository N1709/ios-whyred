#pragma once
#include <stdint.h>

/* whyred (SDM636): splash_region at 0x9d400000 */
#define FB_PHYS		0x9d400000UL
#define FB_PHYS_SWEET	0x9c000000UL

struct mmu_info {
	uint64_t img_lo, img_hi, ttbr;
	uint64_t sctlr_before, sctlr_after;
	int n_l3;
	int page_granule;	/* 4 or 16 (KB) */
};

int mmu_enable(struct mmu_info *info);
uint64_t mmu_probe(uintptr_t va);
