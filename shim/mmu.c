/*
 * Milestone 3: EL1 stage-1 MMU.
 *
 * Supports both 4K granule (whyred / SDM636: Cortex-A53/A73 Kryo 260) and
 * 16K granule (sweet / SM7150: Cortex-A76/A55 Kryo 470).
 *
 * On whyred (TGran16 = 0 in ID_AA64MMFR0_EL1):
 *   T0SZ=28 gives 36-bit VA (64GB). 4K granule walks start at level 1:
 *   L1 table maps 64 1GB regions. Level 2 tables map 2MB blocks identity mapped.
 *   Image regions get Level 3 tables (4KB pages Normal WB), splash FB is Normal NC.
 *
 * On sweet (TGran16 supported):
 *   T0SZ=28 gives 36-bit VA. 16K granule walks start at level 2:
 *   one 16K table of 2048 x 32MB blocks covers 64GB identity mapped.
 *
 * Qualcomm XPUs fault on speculative reads of protected carve-outs (TZ,
 * hyp, modem...), so nothing is mapped cacheable except our own image:
 *   - default: Device-nGnRnE, execute-never (same as MMU off)
 *   - the block(s) holding the image get an L3 table: image pages are
 *     Normal write-back, the rest stays Device
 *   - the splash framebuffer block is Normal non-cacheable (write-combining)
 */
#include <stdint.h>
#include "mmu.h"

#define PG_SHIFT_16K	14
#define PG_SIZE_16K	(1UL << PG_SHIFT_16K)		/* 16K */
#define BLK_SHIFT_16K	25
#define BLK_SIZE_16K	(1UL << BLK_SHIFT_16K)		/* 32MB level-2 block */
#define NENT_16K	2048

#define PG_SHIFT_4K	12
#define PG_SIZE_4K	(1UL << PG_SHIFT_4K)		/* 4K */
#define BLK_SHIFT_4K	21
#define BLK_SIZE_4K	(1UL << BLK_SHIFT_4K)		/* 2MB level-2 block */
#define NENT_4K		512

#define ATTR_DEV	0	/* MAIR attr0 = 0x00 Device-nGnRnE */
#define ATTR_WB		1	/* MAIR attr1 = 0xff Normal WB RW-alloc */
#define ATTR_NC		2	/* MAIR attr2 = 0x44 Normal non-cacheable */
#define MAIR_VAL	0x44ff00UL

#define D_BLOCK		1UL
#define D_TABLE		3UL
#define D_PAGE		3UL
#define D_ATTR(i)	((uint64_t)(i) << 2)
#define D_SH_OUTER	(2UL << 8)
#define D_SH_INNER	(3UL << 8)
#define D_AF		(1UL << 10)
#define D_PXN		(1UL << 53)
#define D_UXN		(1UL << 54)

#define DEV_FLAGS	(D_ATTR(ATTR_DEV) | D_AF | D_PXN | D_UXN)
#define WB_FLAGS	(D_ATTR(ATTR_WB) | D_SH_INNER | D_AF | D_UXN)
#define NC_FLAGS	(D_ATTR(ATTR_NC) | D_SH_OUTER | D_AF | D_PXN | D_UXN)

/* TCR 16K: T0SZ=28, IRGN0/ORGN0=WBWA, SH0=inner, TG0=16K, EPD1, TG1=16K, IPS=36 bit */
#define TCR_VAL_16K	(28UL | (1UL << 8) | (1UL << 10) | (3UL << 12) | (2UL << 14) | \
			 (28UL << 16) | (1UL << 23) | (1UL << 30) | (1UL << 32))

/* TCR 4K: T0SZ=28, IRGN0/ORGN0=WBWA, SH0=inner, TG0=4K (00), EPD1, IPS=36 bit */
#define TCR_VAL_4K	(28UL | (1UL << 8) | (1UL << 10) | (3UL << 12) | (0UL << 14) | \
			 (28UL << 16) | (1UL << 23) | (1UL << 32))

#define SCTLR_M		(1UL << 0)
#define SCTLR_A		(1UL << 1)
#define SCTLR_C		(1UL << 2)
#define SCTLR_I		(1UL << 12)
#define SCTLR_WXN	(1UL << 19)

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))

extern char _head[], _end[];

/* Page table pool: covers up to 16K tables (slack to align to 16K/4K) */
static uint64_t pt_pool[8 * 1024] __attribute__((aligned(4096)));

static void dcache_inval_range(uintptr_t start, uintptr_t end)
{
	uint64_t line = 4UL << ((rsr("ctr_el0") >> 16) & 0xf);

	for (uintptr_t a = start & ~(line - 1); a < end; a += line)
		__asm__ volatile("dc ivac, %0" : : "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static void zero_words(uint64_t *t, int count)
{
	for (int i = 0; i < count; i++)
		t[i] = 0;
}

int mmu_enable(struct mmu_info *info)
{
	uint64_t mmfr0 = rsr("id_aa64mmfr0_el1");
	int has_16k = ((mmfr0 >> 20) & 0xf) == 1;

	if (has_16k) {
		/* 16K Granule Path (sweet SM7150) */
		info->page_granule = 16;
		uintptr_t img_lo = (uintptr_t)_head & ~(PG_SIZE_16K - 1);
		uintptr_t img_hi = ((uintptr_t)_end + PG_SIZE_16K - 1) & ~(PG_SIZE_16K - 1);
		uintptr_t pool = ((uintptr_t)pt_pool + PG_SIZE_16K - 1) & ~(PG_SIZE_16K - 1);
		uint64_t *l2 = (uint64_t *)pool;
		int n_l3 = 0;

		info->img_lo = img_lo;
		info->img_hi = img_hi;
		info->ttbr = pool;
		if (((img_hi - 1) >> BLK_SHIFT_16K) - (img_lo >> BLK_SHIFT_16K) > 1)
			return -1;

		zero_words(l2, NENT_16K);
		for (uint64_t i = 0; i < (8UL << 30) >> BLK_SHIFT_16K; i++)
			l2[i] = (i << BLK_SHIFT_16K) | DEV_FLAGS | D_BLOCK;
		l2[FB_PHYS >> BLK_SHIFT_16K] = (FB_PHYS & ~(BLK_SIZE_16K - 1)) | NC_FLAGS | D_BLOCK;

		for (uint64_t blk = img_lo >> BLK_SHIFT_16K; blk <= (img_hi - 1) >> BLK_SHIFT_16K; blk++) {
			uint64_t *l3 = (uint64_t *)(pool + (1 + n_l3++) * PG_SIZE_16K);
			uint64_t base = blk << BLK_SHIFT_16K;
			for (int i = 0; i < NENT_16K; i++) {
				uint64_t pa = base + ((uint64_t)i << PG_SHIFT_16K);
				int ours = pa >= img_lo && pa < img_hi;
				l3[i] = pa | (ours ? WB_FLAGS : DEV_FLAGS) | D_PAGE;
			}
			l2[blk] = (uintptr_t)l3 | D_TABLE;
		}
		info->n_l3 = n_l3;

		dcache_inval_range(img_lo, img_hi);
		__asm__ volatile("ic iallu; dsb sy; isb" ::: "memory");

		wsr("mair_el1", MAIR_VAL);
		wsr("tcr_el1", TCR_VAL_16K);
		wsr("ttbr0_el1", pool);
	} else {
		/* 4K Granule Path (whyred SDM636: Cortex-A53 / Cortex-A73) */
		info->page_granule = 4;
		uintptr_t img_lo = (uintptr_t)_head & ~(PG_SIZE_4K - 1);
		uintptr_t img_hi = ((uintptr_t)_end + PG_SIZE_4K - 1) & ~(PG_SIZE_4K - 1);
		uintptr_t pool = ((uintptr_t)pt_pool + PG_SIZE_4K - 1) & ~(PG_SIZE_4K - 1);
		uint64_t *l1 = (uint64_t *)pool;
		uintptr_t next_free = pool + PG_SIZE_4K;
		int n_l3 = 0;

		info->img_lo = img_lo;
		info->img_hi = img_hi;
		info->ttbr = pool;

		zero_words(l1, NENT_4K);

		/* 4 L2 tables to cover first 4GB (or 8 for 8GB) */
		for (uint64_t gb = 0; gb < 8; gb++) {
			uint64_t *l2 = (uint64_t *)next_free;
			next_free += PG_SIZE_4K;
			zero_words(l2, NENT_4K);
			l1[gb] = (uintptr_t)l2 | D_TABLE;

			for (uint64_t mb = 0; mb < 512; mb++) {
				uint64_t pa = (gb << 30) | (mb << BLK_SHIFT_4K);
				uint64_t flags = DEV_FLAGS;
				if (pa == (FB_PHYS & ~(BLK_SIZE_4K - 1)))
					flags = NC_FLAGS;
				l2[mb] = pa | flags | D_BLOCK;
			}
		}

		/* L3 for 2MB blocks containing image */
		for (uint64_t blk = img_lo >> BLK_SHIFT_4K; blk <= (img_hi - 1) >> BLK_SHIFT_4K; blk++) {
			uint64_t gb = blk >> 9;
			uint64_t mb = blk & 0x1ff;
			uint64_t *l2 = (uint64_t *)(l1[gb] & ~0xfffUL);
			uint64_t *l3 = (uint64_t *)next_free;
			next_free += PG_SIZE_4K;
			n_l3++;

			uint64_t base = blk << BLK_SHIFT_4K;
			for (int i = 0; i < NENT_4K; i++) {
				uint64_t pa = base + ((uint64_t)i << PG_SHIFT_4K);
				int ours = pa >= img_lo && pa < img_hi;
				l3[i] = pa | (ours ? WB_FLAGS : DEV_FLAGS) | D_PAGE;
			}
			l2[mb] = (uintptr_t)l3 | D_TABLE;
		}
		info->n_l3 = n_l3;

		dcache_inval_range(img_lo, img_hi);
		__asm__ volatile("ic iallu; dsb sy; isb" ::: "memory");

		wsr("mair_el1", MAIR_VAL);
		wsr("tcr_el1", TCR_VAL_4K);
		wsr("ttbr0_el1", pool);
	}

	__asm__ volatile("isb; tlbi vmalle1; dsb nsh; isb" ::: "memory");

	uint64_t sctlr = rsr("sctlr_el1");
	info->sctlr_before = sctlr;
	sctlr &= ~(SCTLR_A | SCTLR_WXN);
	sctlr |= SCTLR_M | SCTLR_C | SCTLR_I;
	wsr("sctlr_el1", sctlr);
	__asm__ volatile("isb" ::: "memory");
	info->sctlr_after = rsr("sctlr_el1");
	return 0;
}

/* Translate a VA with AT S1E1R; returns PAR_EL1. */
uint64_t mmu_probe(uintptr_t va)
{
	__asm__ volatile("at s1e1r, %0; isb" : : "r"(va) : "memory");
	return rsr("par_el1");
}
