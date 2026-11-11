/*
 * Milestone 3: EL1 stage-1 identity MMU (VA == PA), 36-bit VA (T0SZ=28), 64GB.
 *
 * Granule per board (board.h):
 *   16K (SDM845/SM7150): the walk starts at level 2, one table of 2048 x 32MB blocks, level 3 = 16K pages.
 *   4K  (SDM630, Cortex-A53 class, MMU_GRANULE_4K: no 16K granule in ID_AA64MMFR0): the walk starts at
 *       level 1 (64 x 1GB blocks), level 2 = 2MB blocks, level 3 = 4K pages.
 * Both are built by the same refine-on-demand code: set_range() writes the largest blocks a range fully
 * covers and splits a block into a next-level table (inheriting its attributes) where it only partly does.
 *
 * Qualcomm XPUs fault on speculative reads of protected carve-outs (TZ, hyp, modem...), so nothing is
 * mapped cacheable except our own image and the ranges a caller hands in:
 *   - default: Device-nGnRnE, execute-never (same as MMU off), the first 8GB
 *   - the splash framebuffer is Normal non-cacheable (write-combining): with 16K the whole 32MB block
 *     around it (unchanged from the SDM845 bring-up), with 4K only the framebuffer's own pages
 *   - the image and the extra ranges: Normal write-back, rounded out to MMU_PAGE
 *
 * The table builder is pure C over memory: mmu_test.c builds tables for both granules on the host and
 * checks every page with a software walk.
 */
#include <stdint.h>
#include "mmu.h"

#if defined(MMU_GRANULE_4K)
#define PG_SHIFT	12
#define TOP_SHIFT	30				/* level 1: 1GB blocks */
#define NLEVELS		3
/* TCR: T0SZ=28, IRGN0/ORGN0=WBWA, SH0=inner, TG0=4K (0), EPD1, TG1=4K (2), IPS=36 bit */
#define TCR_VAL		(28UL | (1UL << 8) | (1UL << 10) | (3UL << 12) | (0UL << 14) | \
			 (28UL << 16) | (1UL << 23) | (2UL << 30) | (1UL << 32))
#else
#define PG_SHIFT	14
#define TOP_SHIFT	25				/* level 2: 32MB blocks */
#define NLEVELS		2
/* TCR: T0SZ=28, IRGN0/ORGN0=WBWA, SH0=inner, TG0=16K, EPD1, TG1=16K, IPS=36 bit */
#define TCR_VAL		(28UL | (1UL << 8) | (1UL << 10) | (3UL << 12) | (2UL << 14) | \
			 (28UL << 16) | (1UL << 23) | (1UL << 30) | (1UL << 32))
#endif
#define PG_SIZE		(1UL << PG_SHIFT)
#define NENT		(PG_SIZE / 8)
#define LVL_SHIFT(l)	(TOP_SHIFT - (l) * (PG_SHIFT - 3))

#define ATTR_DEV	0	/* MAIR attr0 = 0x00 Device-nGnRnE */
#define ATTR_WB		1	/* MAIR attr1 = 0xff Normal WB RW-alloc */
#define ATTR_NC		2	/* MAIR attr2 = 0x44 Normal non-cacheable */
#define MAIR_VAL	0x44ff00UL

#define D_VALID		1UL
#define D_BLOCK		1UL
#define D_TABLE		3UL
#define D_PAGE		3UL
#define D_ATTR(i)	((uint64_t)(i) << 2)
#define D_SH_OUTER	(2UL << 8)
#define D_SH_INNER	(3UL << 8)
#define D_AF		(1UL << 10)
#define D_PXN		(1UL << 53)
#define D_UXN		(1UL << 54)
#define D_ATTR_MASK	(0xfff0000000000ffcUL)		/* lower [11:2] + upper [63:52] attributes */
#define D_ADDR_MASK	(0x0000fffffffff000UL)

#define DEV_FLAGS	(D_ATTR(ATTR_DEV) | D_AF | D_PXN | D_UXN)
#define WB_FLAGS	(D_ATTR(ATTR_WB) | D_SH_INNER | D_AF | D_UXN)
#define NC_FLAGS	(D_ATTR(ATTR_NC) | D_SH_OUTER | D_AF | D_PXN | D_UXN)

#define SCTLR_M		(1UL << 0)
#define SCTLR_A		(1UL << 1)
#define SCTLR_C		(1UL << 2)
#define SCTLR_I		(1UL << 12)
#define SCTLR_WXN	(1UL << 19)

#define FB_BYTES	((uint64_t)FB_STRIDE * FB_H)

struct pt {
	uintptr_t pool;		/* first table, PG_SIZE aligned */
	int used, max;
};

/*
 * Give [lo, hi) (multiples of PG_SIZE) the attributes flags in the table t of level lvl that maps
 * [base, base + NENT << shift). Blocks the range fully covers are rewritten; blocks it partly covers are
 * split into a next-level table that keeps the block's old attributes. -1 = out of tables.
 */
static int set_range(struct pt *pt, uint64_t *t, int lvl, uint64_t base, uint64_t lo, uint64_t hi, uint64_t flags)
{
	unsigned shift = LVL_SHIFT(lvl);
	uint64_t bsz = 1UL << shift;
	int last = lvl == NLEVELS - 1;
	uint64_t span_hi = base + ((uint64_t)NENT << shift);

	if (lo < base)
		lo = base;
	if (hi > span_hi)
		hi = span_hi;
	for (uint64_t a = lo & ~(bsz - 1); a < hi; a += bsz) {
		unsigned idx = (unsigned)((a - base) >> shift);
		if (lo <= a && hi >= a + bsz) {
			t[idx] = a | flags | (last ? D_PAGE : D_BLOCK);
			continue;
		}
		if (last)
			return -1;	/* unreachable: ranges are page multiples */
		uint64_t e = t[idx], *sub;
		if ((e & 3) == D_TABLE) {
			sub = (uint64_t *)(uintptr_t)(e & D_ADDR_MASK);
		} else {
			if (pt->used == pt->max)
				return -1;
			sub = (uint64_t *)(pt->pool + (uint64_t)pt->used++ * PG_SIZE);
			unsigned sshift = LVL_SHIFT(lvl + 1);
			int slast = lvl + 1 == NLEVELS - 1;
			for (unsigned i = 0; i < NENT; i++)
				sub[i] = (e & D_VALID) ? (a + ((uint64_t)i << sshift)) | (e & D_ATTR_MASK) |
								 (slast ? D_PAGE : D_BLOCK)
						       : 0;
			t[idx] = (uintptr_t)sub | D_TABLE;
		}
		if (set_range(pt, sub, lvl + 1, a, lo, hi, flags))
			return -1;
	}
	return 0;
}

uintptr_t mmu_build(uintptr_t pool, int max, const struct mmu_range *wb, int n, int *n_tables)
{
	struct pt pt = { pool, 1, max };
	uint64_t *top = (uint64_t *)pool;

	for (unsigned i = 0; i < NENT; i++)
		top[i] = 0;
	if (set_range(&pt, top, 0, 0, 0, 8UL << 30, DEV_FLAGS))
		return 0;
#if defined(MMU_GRANULE_4K)
	uint64_t fb_lo = FB_PHYS & ~(PG_SIZE - 1), fb_hi = (FB_PHYS + FB_BYTES + PG_SIZE - 1) & ~(PG_SIZE - 1);
#else
	uint64_t fb_lo = FB_PHYS & ~((1UL << 25) - 1), fb_hi = fb_lo + (1UL << 25);
#endif
	if (set_range(&pt, top, 0, 0, fb_lo, fb_hi, NC_FLAGS))
		return 0;
	for (int r = 0; r < n; r++) {
		uint64_t lo = wb[r].lo & ~(PG_SIZE - 1), hi = (wb[r].hi + PG_SIZE - 1) & ~(PG_SIZE - 1);
		if (hi > lo && set_range(&pt, top, 0, 0, lo, hi, WB_FLAGS))
			return 0;
	}
	*n_tables = pt.used;
	return pool;
}

#ifndef MMU_HOST_TEST
#if defined(MMU_GRANULE_4K)
#define NTABLES		24
#else
#define NTABLES		12				/* 1 top + 11 L3 (the old MAX_L3 was 10) */
#endif

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))

extern char _head[], _end[];

/* the tables, aligned to PG_SIZE at runtime (one table of slack) */
static uint64_t pt_pool[(NTABLES + 1) * NENT] __attribute__((aligned(4096)));

static void dcache_inval_range(uintptr_t start, uintptr_t end)
{
	uint64_t line = 4UL << ((rsr("ctr_el0") >> 16) & 0xf);

	for (uintptr_t a = start & ~(line - 1); a < end; a += line)
		__asm__ volatile("dc ivac, %0" : : "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

/* The compiled granule is implemented by this CPU (ID_AA64MMFR0_EL1.TGran4 [31:28] / TGran16 [23:20]). */
static int granule_ok(void)
{
	uint64_t mmfr0 = rsr("id_aa64mmfr0_el1");
#if defined(MMU_GRANULE_4K)
	return ((mmfr0 >> 28) & 0xf) != 0xf;
#else
	return ((mmfr0 >> 20) & 0xf) != 0;
#endif
}

int mmu_enable(struct mmu_info *info)
{
	return mmu_enable_ranges(info, 0, 0);
}

int mmu_enable_ranges(struct mmu_info *info, const struct mmu_range *extra, int n_extra)
{
	uintptr_t img_lo = (uintptr_t)_head & ~(PG_SIZE - 1);
	uintptr_t img_hi = ((uintptr_t)_end + PG_SIZE - 1) & ~(PG_SIZE - 1);
	uintptr_t pool = ((uintptr_t)pt_pool + PG_SIZE - 1) & ~(PG_SIZE - 1);
	struct mmu_range wb[4];
	int n = 0, used = 0;

	info->img_lo = img_lo;
	info->img_hi = img_hi;
	info->ttbr = pool;
	info->n_l3 = 0;
	if (n_extra > 3)
		return -1;
	if (!granule_ok())
		return -2;
	wb[n++] = (struct mmu_range){ img_lo, img_hi };
	for (int i = 0; i < n_extra; i++)
		wb[n++] = (struct mmu_range){ extra[i].lo & ~(PG_SIZE - 1),
					      (extra[i].hi + PG_SIZE - 1) & ~(PG_SIZE - 1) };

	if (!mmu_build(pool, NTABLES, wb, n, &used))
		return -1;
	info->n_l3 = used - 1;

	/* Lines may be stale in the cache from ABL; we wrote memory directly. */
	for (int r = 0; r < n; r++)
		dcache_inval_range(wb[r].lo, wb[r].hi);
	__asm__ volatile("ic iallu; dsb sy; isb" ::: "memory");

	wsr("mair_el1", MAIR_VAL);
	wsr("tcr_el1", TCR_VAL);
	wsr("ttbr0_el1", pool);
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
#endif
