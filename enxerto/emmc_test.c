/*
 * Host test of emmc.c + storage.c on 512-byte blocks: a simulated SDHCI 3.0 host (the registers emmc.c
 * may touch, 8/32-bit access rules, ADMA2 32-bit descriptor walk) with an eMMC card behind it (EXT_CSD,
 * CMD8/17/18/24/25/6, Auto-CMD12, R1 status) and a GPT in 512-byte LBAs. Checks command encodings,
 * descriptor tables, completion and error paths, the clock/partition refusals, the read-only build, and
 * storage.c's GPT scan / hash / self test / write+verify over the eMMC blkdev.
 *   cc -DEMMC_HOST_TEST -DBOARD_EVERT [-DEMMC_WRITE=1] emmc.c storage.c blake2b.c emmc_test.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "board.h"
#include "blake2b.h"
#include "emmc.h"
#include "storage.h"

#ifndef EMMC_WRITE
#define EMMC_WRITE 0
#endif

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- fake physical addresses for host pointers (ADMA2 is 32 bit) ---- */
static struct { uintptr_t host; uint64_t pa, size; } reg_map[16];
static unsigned n_map;
static uint64_t next_pa = 0x80000000ULL;

uint64_t emmc_test_dma(const void *p)
{
	uintptr_t h = (uintptr_t)p;
	for (unsigned i = 0; i < n_map; i++)
		if (h >= reg_map[i].host && h < reg_map[i].host + reg_map[i].size)
			return reg_map[i].pa + (h - reg_map[i].host);
	/* emmc.c's own statics: give their neighbourhood a window */
	reg_map[n_map].host = h & ~(uintptr_t)0xfffff;
	reg_map[n_map].size = 0x200000;
	reg_map[n_map].pa = next_pa;
	next_pa += 0x200000;
	n_map++;
	return reg_map[n_map - 1].pa + (h - reg_map[n_map - 1].host);
}

static void map_region(void *p, uint64_t size)
{
	reg_map[n_map].host = (uintptr_t)p;
	reg_map[n_map].size = size;
	reg_map[n_map].pa = next_pa;
	next_pa += (size + 0xfffff) & ~0xfffffULL;
	n_map++;
}

static uint8_t *pa_host(uint64_t pa, uint64_t len)
{
	for (unsigned i = 0; i < n_map; i++)
		if (pa >= reg_map[i].pa && pa + len <= reg_map[i].pa + reg_map[i].size)
			return (uint8_t *)(reg_map[i].host + (pa - reg_map[i].pa));
	return 0;
}

static uint64_t fake_us;
uint64_t emmc_test_us(void)
{
	return fake_us += 10;
}

/* ---- the card ---- */
#define BACK_SECTORS	(96u << 11)		/* 96 MiB backed */
#define CARD_SECTORS	0x7700000u		/* ~61 GB reported */
static uint8_t *disk;
static uint8_t card_ext_csd[512];

/* ---- the host ---- */
static struct {
	uint32_t blk, arg, xfer_cmd, resp0, resp3, int_st, int_en, sig_en, adma_lo, adma_hi;
	uint8_t hc1, tout;
	int pending_polls;	/* R_INT reads before the command completes */
	uint32_t pending_bits;
	unsigned resets, accesses, gcc_reads;
	/* knobs */
	uint32_t gcc_cbcr, clk, caps, version, cqcfg;
	int inject;		/* 1 cmd timeout, 2 data CRC, 3 R1 out of range, 4 never completes, 5 CMD12 error */
	int busy_polls;
	/* log */
	unsigned ncmd[64], blocks_moved;
	int last_idx;
} h;

static void complete(uint32_t bits)
{
	h.pending_bits = bits;
	h.pending_polls = h.busy_polls;
}

static void run_data(int idx, int read)
{
	uint32_t n = h.blk >> 16, bs = h.blk & 0xfff, tm = h.xfer_cmd & 0xffff;
	int multi = idx == 18 || idx == 25;

	CHECK(bs == 512);
	CHECK(n >= 1);
	CHECK((h.hc1 & 0x18) == 0x10);			/* ADMA2 32-bit selected */
	CHECK(tm & 1);					/* DMA */
	CHECK(tm & 2);					/* block count enable */
	CHECK(!!(tm & 0x10) == read);			/* direction */
	CHECK(!!(tm & 0x20) == multi);			/* multi block */
	CHECK(!!(tm & 0x04) == multi);			/* auto CMD12 exactly for multi block */
	CHECK(multi ? n > 1 : n == 1);
	CHECK(((h.xfer_cmd >> 16) & 0x3f) == 0x3a);	/* data present + R1 */
	if (h.inject == 2) {
		complete(1u | 1u << 15 | 1u << 21);	/* cmd done, error: data CRC */
		h.inject = 0;
		return;
	}
	uint64_t lba = idx == 8 ? 0 : h.arg;
	uint32_t r1 = 4u << 9 | 1u << 8;			/* tran, ready */
	if (idx != 8 && lba + n > CARD_SECTORS)
		r1 |= 1u << 31;				/* OUT_OF_RANGE */
	if (h.inject == 3) {
		r1 |= 1u << 31;
		h.inject = 0;
	}
	/* walk the ADMA2 table */
	uint64_t tpa = h.adma_lo | (uint64_t)h.adma_hi << 32, total = 0, done = 0;
	for (unsigned d = 0;; d++) {
		uint8_t *dp = pa_host(tpa + 8ULL * d, 8);
		CHECK(dp != 0);
		if (!dp)
			return;
		uint32_t al, addr;
		memcpy(&al, dp, 4);
		memcpy(&addr, dp + 4, 4);
		uint32_t len = al >> 16;
		CHECK(al & 1);				/* valid */
		CHECK(((al >> 4) & 3) == 2);		/* tran */
		CHECK(len != 0 && !(addr & 3));
		uint8_t *mem = pa_host(addr, len);
		CHECK(mem != 0);
		if (!mem)
			return;
		for (uint32_t o = 0; o < len; o += 512, done++) {
			uint64_t s = lba + done;
			if (idx == 8)
				memcpy(mem + o, card_ext_csd, 512);
			else if (read)
				s < BACK_SECTORS ? memcpy(mem + o, disk + s * 512, 512) : memset(mem + o, 0, 512);
			else if (!(r1 >> 31)) {
				CHECK(s < BACK_SECTORS);
				if (s < BACK_SECTORS)
					memcpy(disk + s * 512, mem + o, 512);
			}
		}
		total += len;
		if (al & 2)
			break;
		CHECK(d < 4096);
		if (d >= 4096)
			return;
	}
	CHECK(total == (uint64_t)n * 512);
	h.blocks_moved += n;
	h.resp0 = r1;
	h.resp3 = multi ? (4u << 9 | 1u << 8 | (h.inject == 5 ? 1u << 31 : 0)) : 0;	/* 5: CMD12 reports OUT_OF_RANGE */
	if (h.inject == 5)
		h.inject = 0;
	complete(3);
}

static void run_cmd(void)
{
	int idx = (h.xfer_cmd >> 24) & 0x3f;
	uint32_t c = h.xfer_cmd >> 16;

	h.last_idx = idx;
	h.ncmd[idx]++;
	if (h.inject == 1) {
		complete(1u << 15 | 1u << 16);		/* error: cmd timeout */
		h.inject = 0;
		return;
	}
	if (h.inject == 4) {
		h.pending_bits = 0;
		h.pending_polls = 1 << 30;
		return;
	}
	switch (idx) {
	case 8: run_data(8, 1); break;
	case 17: case 18: run_data(idx, 1); break;
	case 24: case 25: run_data(idx, 0); break;
	case 6:
		CHECK((c & 0x3f) == 0x1b);		/* R1b, no data */
		CHECK(h.arg == (3u << 24 | 32u << 16 | 1u << 8));
		h.resp0 = 4u << 9 | 1u << 8;
		complete(3);
		break;
	default:
		CHECK(!"unexpected command");
		complete(1u | 1u << 15 | 1u << 19);
	}
}

uint32_t emmc_test_rd(uintptr_t a)
{
	if (a == 0x00100000UL + 0x16004 || a == 0x00100000UL + 0x16008) {
		h.gcc_reads++;
		return h.gcc_cbcr;
	}
	CHECK(a >= EMMC_BASE && a < EMMC_BASE + 0x2000 && !(a & 3));
	h.accesses++;
	switch (a - EMMC_BASE) {
	case 0x04: return h.blk;
	case 0x08: return h.arg;
	case 0x0c: return h.xfer_cmd;
	case 0x10: return h.resp0;
	case 0x1c: return h.resp3;
	case 0x24: return 0;			/* nothing inhibited (commands complete instantly) */
	case 0x28: return h.hc1 | 1u << 8 | 0x0eu << 9;	/* bus power on, 1.8 V */
	case 0x2c: return h.clk | (uint32_t)h.tout << 16;
	case 0x30:
		if (h.pending_bits) {
			if (h.pending_polls > 0)
				h.pending_polls--;
			else {
				h.int_st |= h.pending_bits;
				h.pending_bits = 0;
			}
		}
		return h.int_st;
	case 0x34: return h.int_en;
	case 0x38: return h.sig_en;
	case 0x3c: return 0;
	case 0x40: return h.caps;
	case 0x58: return h.adma_lo;
	case 0x5c: return h.adma_hi;
	case 0xfc: return h.version << 16;
	case 0x1008: return h.cqcfg;
	}
	CHECK(!"read of an unmodelled SDHCI register");
	return 0;
}

void emmc_test_wr(uintptr_t a, uint32_t v, unsigned size)
{
	CHECK(a >= EMMC_BASE && a < EMMC_BASE + 0x100);
	h.accesses++;
	uintptr_t off = a - EMMC_BASE;
	if (size == 1) {
		switch (off) {
		case 0x28: h.hc1 = (uint8_t)v; return;
		case 0x2e: h.tout = (uint8_t)v; return;
		case 0x2f:
			CHECK(!(v & 1));		/* never RESET_ALL (msm power irq handshake) */
			h.resets++;
			h.pending_bits = 0;
			return;
		}
		CHECK(!"8-bit write to an unexpected register");
		return;
	}
	CHECK(!(a & 3));
	switch (off) {
	case 0x04: h.blk = v; return;
	case 0x08: h.arg = v; return;
	case 0x0c: h.xfer_cmd = v; run_cmd(); return;
	case 0x30: h.int_st &= ~v; return;
	case 0x34: h.int_en = v; return;
	case 0x38: h.sig_en = v; return;
	case 0x58: h.adma_lo = v; return;
	case 0x5c: h.adma_hi = v; return;
	case 0x28: CHECK(!"32-bit write over POWER_CONTROL"); return;
	case 0x2c: CHECK(!"32-bit write over clock control"); return;
	}
	CHECK(!"write to an unmodelled SDHCI register");
}

/* ---- GPT in 512-byte LBAs ---- */
static uint32_t crc32_ref(const uint8_t *p, uint64_t n)
{
	uint32_t c = 0xffffffffu;
	for (uint64_t i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
	}
	return ~c;
}
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

#define UD_FIRST	40960u		/* 20 MiB */
#define UD_LAST		(UD_FIRST + 81920u - 1)	/* 40 MiB */
#define BOOT_FIRST	2048u
#define BOOT_LAST	(2048u + 16383u)

static void write_gpt(void)
{
	const char *names[] = { "xbl_a", "boot_a", "boot", "userdata" };
	const uint64_t r[][2] = { { 34, 2000 }, { BOOT_FIRST, BOOT_LAST }, { 18432, 20479 }, { UD_FIRST, UD_LAST } };
	uint8_t *hdr = disk + 512, *ents = disk + 1024;
	memset(disk, 0, 34 * 512);
	for (int i = 0; i < 4; i++) {
		uint8_t *e = ents + 128 * i;
		e[0] = 0xaf;
		e[16] = (uint8_t)(i + 1);
		put64(e + 32, r[i][0]);
		put64(e + 40, r[i][1]);
		for (int k = 0; names[i][k]; k++)
			e[56 + 2 * k] = (uint8_t)names[i][k];
	}
	memcpy(hdr, "EFI PART", 8);
	put32(hdr + 8, 0x10000);
	put32(hdr + 12, 92);
	put64(hdr + 24, 1);
	put64(hdr + 32, CARD_SECTORS - 1);
	put64(hdr + 40, 34);
	put64(hdr + 48, CARD_SECTORS - 34);
	put64(hdr + 72, 2);
	put32(hdr + 80, 128);
	put32(hdr + 84, 128);
	put32(hdr + 88, crc32_ref(ents, 128 * 128));
	put32(hdr + 16, crc32_ref(hdr, 92));
}

static void reset_host(void)
{
	memset(&h, 0, sizeof h);
	h.clk = 1u | 1u << 1 | 1u << 2;
	h.caps = 1u << 19;
	h.version = 0x8a02;			/* vendor 0x8a, spec 3.00 */
	h.hc1 = 0x24;				/* 8-bit bus, high speed, SDMA as ABL may leave it */
}

static void reset_card(void)
{
	memset(card_ext_csd, 0, sizeof card_ext_csd);
	put32(card_ext_csd + 212, CARD_SECTORS);
	card_ext_csd[192] = 8;
	card_ext_csd[183] = 6;			/* 8-bit DDR */
	card_ext_csd[185] = 3;			/* HS400 */
	card_ext_csd[33] = 1;			/* cache on */
}

int main(void)
{
	printf("emmc_test: %s, EMMC_WRITE=%d\n", BOARD_NAME, EMMC_WRITE);
	disk = calloc(BACK_SECTORS, 512);
	for (uint64_t i = 0; i < (uint64_t)BACK_SECTORS * 512; i += 8)
		put64(disk + i, i * 0x9e3779b97f4a7c15ULL);
	write_gpt();
	uint8_t *arena;
	if (posix_memalign((void **)&arena, 4096, 32u << 20))
		return 2;
	map_region(arena, 32u << 20);

	/* refusals first (emmc_init is idempotent once it succeeded) */
	reset_host();
	reset_card();
	h.gcc_cbcr = 1u << 31;
	CHECK(emmc_init() == EMMC_E_NOTREADY);
	CHECK(h.gcc_reads > 0 && h.accesses == 0);	/* nothing touched behind an unclocked host */
	reset_host();
	h.clk = 1u | 1u << 1;				/* SD clock off */
	CHECK(emmc_init() == EMMC_E_NOTREADY);
	reset_host();
	h.cqcfg = 1;
	CHECK(emmc_init() == EMMC_E_NOTREADY);
	reset_host();
	card_ext_csd[179] = 1;				/* boot partition 1 selected */
	CHECK(emmc_init() == EMMC_E_PART);
	reset_card();
	reset_host();
	h.inject = 1;
	CHECK(emmc_init() == EMMC_E_CMD && h.resets == 1);

	/* the happy path */
	reset_host();
	h.busy_polls = 3;
	CHECK(emmc_init() == 0);
	CHECK(emmc_info.sectors == CARD_SECTORS && emmc_info.cache_ctrl == 1 && emmc_info.hs_timing == 3);
	CHECK((h.hc1 & 0x18) == 0x10 && (h.hc1 & ~0x18) == (0x24 & ~0x18));	/* only the DMA select changed */
	CHECK(h.tout == 0x0e && h.sig_en == 0 && !(h.int_en & (1u << 20)) && (h.int_en & (1u << 24)));
	CHECK(h.ncmd[8] == 1);

	/* raw reads: single, multi, multi-command, async */
	uint8_t *buf = arena + (8u << 20);
	CHECK(emmc_read(5, 1, buf) == 0 && !memcmp(buf, disk + 5 * 512, 512) && h.last_idx == 17);
	CHECK(emmc_read(1000, 300, buf) == 0 && !memcmp(buf, disk + 1000 * 512, 300 * 512) && h.last_idx == 18);
	unsigned c18 = h.ncmd[18];
	CHECK(emmc_read(4096, 40000, buf) == 0 && !memcmp(buf, disk + 4096 * 512, 40000ull * 512));
	CHECK(h.ncmd[18] - c18 == 3);			/* 16384 + 16384 + 7232 blocks */
	CHECK(emmc_read_start(77, 64, buf) == 0 && emmc_read_start(1, 1, buf + 65536) == EMMC_E_ARGS);
	CHECK(emmc_read_wait() == 0 && !memcmp(buf, disk + 77 * 512, 64 * 512));
	/* argument checks */
	CHECK(emmc_read(0, 1, buf + 4) == EMMC_E_ARGS);
	CHECK(emmc_read(CARD_SECTORS - 1, 2, buf) == EMMC_E_ARGS);
	CHECK(emmc_read(0, 0, buf) == EMMC_E_ARGS);
	/* error paths recover */
	unsigned r0 = h.resets;
	h.inject = 2;
	CHECK(emmc_read(10, 8, buf) == EMMC_E_DATA && h.resets == r0 + 1);
	h.inject = 3;
	CHECK(emmc_read(10, 8, buf) == EMMC_E_CARD);
	h.inject = 5;
	CHECK(emmc_read(10, 8, buf) == EMMC_E_CARD);
	h.inject = 4;
	CHECK(emmc_read(10, 8, buf) == EMMC_E_TIMEOUT && h.resets == r0 + 2);
	h.inject = 0;
	CHECK(emmc_read(10, 8, buf) == 0 && !memcmp(buf, disk + 10 * 512, 8 * 512));

	/* storage.c over the eMMC blkdev: 512-byte GPT */
	static struct stor st;
	memset(&st, 0, sizeof st);
	emmc_blkdev(&st.bd);
	st.scratch = arena;
	CHECK(stor_blksz(&st) == 512);
	CHECK(stor_scan(&st) == 0);
	CHECK(st.luns_found == 1 && st.lun_info[0].valid && st.lun_info[0].entries == 128);
	const char *why = 0;
	const struct stor_part *ud = stor_find(&st, "userdata", &why);
	CHECK(ud && ud->first == UD_FIRST && ud->nblk == UD_LAST - UD_FIRST + 1);
	CHECK(ud && stor_part_bytes(&st, ud) == 40ull << 20);
	const struct stor_part *bt = stor_find(&st, "boot", &why);
	CHECK(bt && bt->first == 18432);
	CHECK(!stor_find(&st, "boot_a", &why));		/* not on the allowlist */
	CHECK(!stor_find(&st, "xbl_a", &why));

	/* hash: BLAKE2b-128 per 1 MiB block of the first 6 MiB of userdata, against the disk bytes */
	uint8_t hash[6 * 16], ref[16];
	CHECK(stor_hash(&st, ud, 0, 6u << 20, 1u << 20, hash) == 0);
	int hash_ok = 1;
	for (int i = 0; i < 6; i++) {
		blake2b(ref, 16, disk + (uint64_t)UD_FIRST * 512 + ((uint64_t)i << 20), 1u << 20);
		hash_ok &= !memcmp(ref, hash + 16 * i, 16);
	}
	CHECK(hash_ok);
	CHECK(stor_selftest(&st, ud) == 0);

	/* writes */
	uint8_t *src = arena + (16u << 20);
	for (unsigned i = 0; i < (10u << 20); i++)
		src[i] = (uint8_t)(i * 7 + 3);
	unsigned w0 = h.ncmd[24] + h.ncmd[25];
	(void)w0;
	uint8_t before[512];
	memcpy(before, disk + (uint64_t)UD_FIRST * 512 + (1u << 20), 512);
	st.verify = 1;
	why = stor_write(&st, ud, 1u << 20, src, 10u << 20);
#if EMMC_WRITE
	CHECK(why == 0);
	CHECK(!memcmp(disk + (uint64_t)UD_FIRST * 512 + (1u << 20), src, 10u << 20));
	CHECK(h.ncmd[25] > 0);
	CHECK(stor_sync(&st, ud) == 0 && h.ncmd[6] == 1);
	CHECK(emmc_write(CARD_SECTORS, 1, src) == EMMC_E_ARGS);
	CHECK(emmc_write(UD_FIRST, 1, src) == 0 && h.last_idx == 24);
#else
	CHECK(why && strstr(why, "EMMC_WRITE"));
	CHECK(h.ncmd[24] + h.ncmd[25] == w0);
	CHECK(!memcmp(disk + (uint64_t)UD_FIRST * 512 + (1u << 20), before, 512));
#endif
	/* sync with the card cache off issues nothing */
	emmc_info.cache_ctrl = 0;
	unsigned c6 = h.ncmd[6];
	CHECK(emmc_sync() == 0 && h.ncmd[6] == c6);
	/* the protocol granularity stays 4 KiB */
	CHECK(stor_write(&st, ud, 512, src, 4096) != 0);

	printf("emmc_test: %d checks, %d failures\n", checks, fails);
	return fails != 0;
}
