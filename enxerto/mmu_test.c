/*
 * Host test of mmu.c's table builder (mmu_build) for the board's granule: every page of the first 9GB is
 * translated with a software walk of the built tables and must have the expected memory type:
 * write-back inside a (page-rounded) wb range, non-cacheable in the framebuffer window, Device elsewhere
 * below 8GB, unmapped above. Also checks output address == input (identity), descriptor types per level,
 * and that realistic loader layouts fit in the firmware's table pool.
 *   cc -DMMU_HOST_TEST -DBOARD_EVERT     mmu.c mmu_test.c   (4K granule)
 *   cc -DMMU_HOST_TEST -DBOARD_BERYLLIUM mmu.c mmu_test.c   (16K granule)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mmu.h"

#if defined(MMU_GRANULE_4K)
#define PG_SHIFT 12
#define TOP_SHIFT 30
#define NLEVELS 3
#define NTABLES 24
#define FB_LO (FB_PHYS & ~(MMU_PAGE - 1))
#define FB_HI ((FB_PHYS + (uint64_t)FB_STRIDE * FB_H + MMU_PAGE - 1) & ~(MMU_PAGE - 1))
#else
#define PG_SHIFT 14
#define TOP_SHIFT 25
#define NLEVELS 2
#define NTABLES 12
#define FB_LO (FB_PHYS & ~((1UL << 25) - 1))
#define FB_HI (FB_LO + (1UL << 25))
#endif
#define PG (1UL << PG_SHIFT)
#define NENT (PG / 8)

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; if (fails < 20) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

/* -1 = unmapped, else MAIR index; *out = output address */
static int walk(uintptr_t ttbr, uint64_t va, uint64_t *out)
{
	uint64_t *t = (uint64_t *)ttbr;
	for (int l = 0; l < NLEVELS; l++) {
		unsigned shift = TOP_SHIFT - l * (PG_SHIFT - 3);
		unsigned idx = (va >> shift) & (l == 0 ? (1u << (36 - TOP_SHIFT)) - 1 : NENT - 1);
		if (l == 0 && (va >> 36))
			return -1;
		uint64_t e = t[idx];
		if (!(e & 1))
			return -1;
		int last = l == NLEVELS - 1;
		if (last) {
			if ((e & 3) != 3)
				return -2;	/* reserved encoding at level 3 */
		} else if ((e & 3) == 3) {
			t = (uint64_t *)(uintptr_t)(e & 0x0000fffffffff000UL & ~(PG - 1));
			continue;
		}
		uint64_t mask = (1UL << shift) - 1;
		*out = (e & 0x0000fffffffff000UL & ~mask) | (va & mask);
		if (!(e & (1UL << 10)))
			return -3;	/* AF clear */
		return (int)((e >> 2) & 7);
	}
	return -4;
}

static int expect(uint64_t pa, const struct mmu_range *wb, int n)
{
	for (int i = 0; i < n; i++)
		if (pa >= (wb[i].lo & ~(PG - 1)) && pa < ((wb[i].hi + PG - 1) & ~(PG - 1)))
			return 1;
	if (pa >= FB_LO && pa < FB_HI)
		return 2;
	return pa < (8UL << 30) ? 0 : -1;
}

static void run(const char *name, const struct mmu_range *wb, int n, int max_tables)
{
	void *mem;
	int used = 0;
	if (posix_memalign(&mem, PG, (size_t)max_tables * PG))
		exit(2);
	memset(mem, 0xee, (size_t)max_tables * PG);	/* garbage: the builder must not rely on zeroed memory */
	uintptr_t ttbr = mmu_build((uintptr_t)mem, max_tables, wb, n, &used);
	CHECK(ttbr == (uintptr_t)mem, "%s: build failed", name);
	if (!ttbr) {
		free(mem);
		return;
	}
	uint64_t bad = 0;
	for (uint64_t va = 0; va < (9UL << 30); va += PG) {
		uint64_t out = ~0UL;
		int got = walk(ttbr, va, &out), want = expect(va, wb, n);
		if (got != want || (got >= 0 && out != va)) {
			if (!bad)
				CHECK(0, "%s: va %#llx got %d want %d out %#llx", name, (unsigned long long)va, got, want,
				      (unsigned long long)out);
			bad++;
		}
	}
	CHECK(!bad, "%s: %llu bad pages", name, (unsigned long long)bad);
	if (bad || strncmp(name, "random", 6))
		printf("  %-34s tables %2d/%d %s\n", name, used, max_tables, bad ? "BAD" : "ok");
	free(mem);
}

int main(void)
{
	printf("mmu_test: %s, %lu-byte granule\n", BOARD_NAME, (unsigned long)PG);
	/* no extra ranges: image only */
	struct mmu_range img[] = { { 0x80080000UL, 0x80080000UL + 0xb2000 } };
	run("image only", img, 1, NTABLES);
	/* the loader: image, kernel destination + ADT + boot_args, payload ramdisk */
	struct mmu_range loader[] = {
		{ 0x80080000UL, 0x80080000UL + 0xb4000 },	/* image */
		{ 0x800e4000UL, 0x88e3c000UL },			/* phys_base .. top_pa (~141MB) */
		{ 0x82000000UL + 0x1234, 0x8f800000UL + 0x777 },	/* ramdisk payload, unaligned ends */
	};
	run("loader layout", loader, 3, NTABLES);
	/* fastboot cached window: image, 4MB scratch, a compared image crossing a 1GB boundary */
	struct mmu_range win[] = {
		{ 0x80080000UL, 0x80134000UL },
		{ 0xa0000000UL, 0xa0400000UL },
		{ 0xbff00000UL + 0x40, 0xc0300000UL - 0x40 },
	};
	run("cached window across 1GB", win, 3, NTABLES);
	/* a write-back range overlapping the framebuffer window: wb wins */
	struct mmu_range fbov[] = { { 0x80080000UL, 0x80134000UL }, { FB_PHYS - 0x10000, FB_PHYS + 0x20000 } };
	run("wb overlapping framebuffer", fbov, 2, NTABLES);
	/* range above 4GB, whole-block coverage */
	struct mmu_range hi[] = { { 0x80080000UL, 0x80134000UL }, { 0x100000000UL, 0x180000000UL } };
	run("2GB above 4GB (whole blocks)", hi, 2, NTABLES);
	/* random ranges in RAM, generous pool */
	srand(1234);
	for (int k = 0; k < 40; k++) {
		struct mmu_range r[4];
		for (int i = 0; i < 4; i++) {
			uint64_t lo = 0x80000000UL + ((uint64_t)rand() % 0x100000000UL);
			uint64_t len = (uint64_t)rand() % ((k & 1) ? 0x100000UL : 0x30000000UL) + 1;
			r[i] = (struct mmu_range){ lo, lo + len };
		}
		char nm[32];
		snprintf(nm, sizeof nm, "random #%d", k);
		run(nm, r, 4, 128);
	}
	/* pool exhaustion must fail cleanly, not overrun */
	{
		void *mem;
		int used = 0;
		if (posix_memalign(&mem, PG, 2 * PG))
			return 2;
		struct mmu_range many[] = { { 0x80001000UL & ~(PG - 1), 0x80001000UL + PG }, { 0x90000000UL + PG, 0x90000000UL + 2 * PG },
					    { 0xa0000000UL + PG, 0xa0000000UL + 2 * PG } };
		CHECK(mmu_build((uintptr_t)mem, 2, many, 3, &used) == 0, "exhausted pool must return 0");
		free(mem);
	}
	printf(fails ? "mmu_test: %d FAILURES\n" : "mmu_test: all passed\n", fails);
	return fails != 0;
}
