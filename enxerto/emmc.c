/*
 * eMMC over the SDHCI host ABL left running: see emmc.h. Register layout: SD Host Controller Simplified
 * Specification 3.00 (the sdhci-msm-v5 "hc_mem" region is the standard register set; the Qualcomm vendor
 * registers are not touched). Quirks taken from the LineageOS msm-4.4 sdhci-msm.c for this host:
 * SDHCI_QUIRK_NO_ENDATTR_IN_NOPDESC (we never emit NOP descriptors), IGNORE_DATATOUT_FOR_R1BCMD (the
 * data-timeout interrupt is masked, a software timeout is used instead).
 *
 * Register writes avoid side effects on the msm power-IRQ handshake: Host Control 1 and the software
 * reset are 8-bit writes (POWER_CONTROL at 0x29 is never written), no RESET_ALL.
 */
#include "board.h"
#include "emmc.h"
#include "storage.h"

#ifndef EMMC_WRITE
#define EMMC_WRITE	0
#endif

/* SDHCI registers */
#define R_ADMA_LO	0x58
#define R_ADMA_HI	0x5c
#define R_BLK		0x04	/* [11:0] block size, [14:12] SDMA boundary, [31:16] block count */
#define R_ARG		0x08
#define R_XFER_CMD	0x0c	/* [15:0] transfer mode, [31:16] command */
#define R_RESP0		0x10
#define R_RESP3		0x1c	/* Auto CMD12 response */
#define R_PSTATE	0x24
#define R_HC1		0x28	/* 8 bit: host control 1 */
#define R_CLK		0x2c	/* [15:0] clock control, [23:16] timeout control, [31:24] software reset */
#define R_TOUT		0x2e	/* 8 bit */
#define R_RESET		0x2f	/* 8 bit */
#define R_INT		0x30	/* [15:0] normal status, [31:16] error status (W1C) */
#define R_INT_EN	0x34
#define R_SIG_EN	0x38
#define R_AUTO_HC2	0x3c	/* [15:0] auto CMD error status, [31:16] host control 2 */
#define R_CAPS		0x40
#define R_VERSION	0xfc	/* [31:16] host controller version */
#define CQ_CFG		0x1008	/* cmdq_mem (EMMC_BASE + 0x1000) CQCFG: bit0 CQ enable */

#define PS_CMD_INH	(1u << 0)
#define PS_DAT_INH	(1u << 1)
#define CLK_INT_STABLE	(1u << 1)
#define CLK_SD_EN	(1u << 2)
#define PWR_ON		(1u << 8)	/* in the R_HC1 word */
#define HC1_DMA_MASK	(3u << 3)
#define HC1_ADMA2_32	(2u << 3)
#define HC2_V4_EN	(1u << 12)
#define CAPS_ADMA2	(1u << 19)
#define RST_CMD		(1u << 1)
#define RST_DAT		(1u << 2)

#define INT_CMD_DONE	(1u << 0)
#define INT_XFER_DONE	(1u << 1)
#define INT_DMA		(1u << 3)
#define INT_ERR		(1u << 15)
#define ERR_CMD_MASK	(0x0fu << 16)		/* cmd timeout, crc, end bit, index */
#define ERR_DATA_TOUT	(1u << 20)
#define ERR_AUTO_CMD	(1u << 24)
/* enable everything but data timeout (sdhci-msm IGNORE_DATATOUT_FOR_R1BCMD; software timeout instead) */
#define INT_EN_VAL	(INT_CMD_DONE | INT_XFER_DONE | INT_DMA | ((0x3ffu & ~(1u << 4)) << 16))

#define TM_DMA		(1u << 0)
#define TM_BLKCNT	(1u << 1)
#define TM_ACMD12	(1u << 2)
#define TM_READ		(1u << 4)
#define TM_MULTI	(1u << 5)

#define C_RESP_R1	0x1au			/* 48 bit, CRC + index check */
#define C_RESP_R1B	0x1bu			/* 48 bit busy */
#define C_DATA		(1u << 5)

/* R1 card status: error bits (JEDEC 84-B51 6.13) and CURRENT_STATE [12:9] */
#define R1_ERRORS	0xfdf98080u
#define R1_STATE(r)	(((r) >> 9) & 0xf)
#define R1_TRAN		4u

/* ADMA2 32-bit descriptor: attr [5:0] (valid, end, int, act[5:4]=2 tran), length 16 bit, address 32 bit */
#define AD_VALID	(1u << 0)
#define AD_END		(1u << 1)
#define AD_TRAN		(2u << 4)
#define AD_CHUNK	0x8000u			/* bytes per descriptor */
#define MAX_XFER	(8u << 20)		/* bytes per command */
#define NDESC		(MAX_XFER / AD_CHUNK)
#define MAX_BLKS	(MAX_XFER / EMMC_BLK)

/* GCC (sdm660 gcc: gcc_sdcc1_apps_clk 0x16004, gcc_sdcc1_ahb_clk 0x16008), bit 31 CLK_OFF */
#define GCC_BASE	0x00100000UL
#define GCC_SDCC1_APPS	0x16004
#define GCC_SDCC1_AHB	0x16008
#define CBCR_OFF	(1u << 31)

#define T_CMD_US	1000000u
#define T_DATA_US	30000000u

#ifdef EMMC_HOST_TEST
/* emmc_test.c: a simulated host + card behind the register addresses and the DMA addresses */
uint32_t emmc_test_rd(uintptr_t a);
void emmc_test_wr(uintptr_t a, uint32_t v, unsigned size);
uint64_t emmc_test_dma(const void *p);
uint64_t emmc_test_us(void);
static inline uint32_t rd_at(uintptr_t a) { return emmc_test_rd(a); }
static inline void wr_at(uintptr_t a, uint32_t v) { emmc_test_wr(a, v, 4); }
static inline void wr8_at(uintptr_t a, uint8_t v) { emmc_test_wr(a, v, 1); }
static inline uint64_t dma_addr(const void *p) { return emmc_test_dma(p); }
static inline uint64_t now_us(void) { return emmc_test_us(); }
static inline void dc_clean(const void *a, uint64_t n) { (void)a; (void)n; }
static inline void dc_flush(const void *a, uint64_t n) { (void)a; (void)n; }
static inline void dc_inval(const void *a, uint64_t n) { (void)a; (void)n; }
#else
#include "cache.h"
static inline uint32_t rd_at(uintptr_t a)
{
	uint32_t v = *(volatile uint32_t *)a;
	__asm__ volatile("dmb sy" ::: "memory");
	return v;
}
static inline void wr_at(uintptr_t a, uint32_t v)
{
	__asm__ volatile("dmb sy" ::: "memory");
	*(volatile uint32_t *)a = v;
	__asm__ volatile("dsb sy" ::: "memory");
}
static inline void wr8_at(uintptr_t a, uint8_t v)
{
	__asm__ volatile("dmb sy" ::: "memory");
	*(volatile uint8_t *)a = v;
	__asm__ volatile("dsb sy" ::: "memory");
}
static inline uint64_t dma_addr(const void *p) { return (uint64_t)(uintptr_t)p; }
static inline uint64_t now_us(void)
{
	uint64_t f, t;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
	return t / (f / 1000000);
}
#endif

static inline uint32_t rd(uint32_t off) { return rd_at(EMMC_BASE + off); }
static inline void wr(uint32_t off, uint32_t v) { wr_at(EMMC_BASE + off, v); }
static inline void wr8(uint32_t off, uint8_t v) { wr8_at(EMMC_BASE + off, v); }

struct emmc_info emmc_info;
uint64_t emmc_blocks_read, emmc_blocks_written;

static struct { uint32_t attr_len, addr; } adma[NDESC] __attribute__((aligned(64)));
static uint8_t ext_csd[EMMC_BLK] __attribute__((aligned(64)));
static int inited;
static void (*idle_fn)(void);

/* the running transfer (emmc_read_start / emmc_read_wait, and the synchronous paths) */
static struct {
	int active, write, cmd_out;
	uint64_t lba, left;	/* blocks not yet issued */
	uint8_t *buf;		/* next byte to issue */
	uint8_t *cbuf;		/* buffer of the command in flight */
	uint32_t cblks;		/* its block count */
	uint64_t t0;
} x;

void emmc_set_idle(void (*fn)(void))
{
	idle_fn = fn;
}

const char *emmc_errstr(int rc)
{
	switch (rc) {
	case 0: return "ok";
	case EMMC_E_NOTREADY: return "eMMC host/card not as ABL should leave it";
	case EMMC_E_TIMEOUT: return "eMMC timeout";
	case EMMC_E_CMD: return "eMMC command error";
	case EMMC_E_DATA: return "eMMC data error";
	case EMMC_E_CARD: return "eMMC card status error";
	case EMMC_E_ARGS: return "eMMC bad arguments";
	case EMMC_E_RO: return "eMMC writes disabled in this build (EMMC_WRITE=1)";
	case EMMC_E_PART: return "eMMC not on the user data area";
	default: return "eMMC ?";
	}
}

static void reset_lines(void)
{
	wr8(R_RESET, RST_CMD | RST_DAT);
	uint64_t t0 = now_us();
	while ((rd(R_CLK) >> 24) & (RST_CMD | RST_DAT))
		if (now_us() - t0 > 100000)
			break;
	wr(R_INT, 0xffffffffu);
}

static int wait_idle(uint32_t mask)
{
	uint64_t t0 = now_us();
	while (rd(R_PSTATE) & mask)
		if (now_us() - t0 > T_CMD_US)
			return EMMC_E_TIMEOUT;
	return 0;
}

/* Data DMA through the ADMA2 table: buf must be 4-byte aligned (callers keep 64) and below 4 GiB. */
static int build_adma(const void *buf, uint32_t len)
{
	uint64_t pa = dma_addr(buf);
	unsigned n = 0;

	if ((pa & 3) || pa + len > (1ULL << 32) || !len || len > MAX_XFER)
		return EMMC_E_ARGS;
	for (uint32_t off = 0; off < len; off += AD_CHUNK, n++) {
		uint32_t l = len - off > AD_CHUNK ? AD_CHUNK : len - off;
		adma[n].attr_len = (l << 16) | AD_TRAN | AD_VALID;
		adma[n].addr = (uint32_t)(pa + off);
	}
	adma[n - 1].attr_len |= AD_END;
	dc_clean(adma, sizeof adma);
	uint64_t ta = dma_addr(adma);
	wr(R_ADMA_LO, (uint32_t)ta);
	wr(R_ADMA_HI, (uint32_t)(ta >> 32));
	return 0;
}

/* Issue a command; data commands get blocks x 512 bytes at buf. Does not wait. */
static int cmd_issue(unsigned idx, uint32_t arg, uint32_t resp, void *buf, uint32_t blocks, int read)
{
	int rc = wait_idle(PS_CMD_INH | ((buf || resp == C_RESP_R1B) ? PS_DAT_INH : 0));
	uint32_t tm = 0, cmd = idx << 8 | resp;

	if (rc)
		return rc;
	wr(R_INT, 0xffffffffu);
	if (buf) {
		if ((rc = build_adma(buf, blocks * EMMC_BLK)))
			return rc;
		if (read)
			dc_flush(buf, (uint64_t)blocks * EMMC_BLK);
		else
			dc_clean(buf, (uint64_t)blocks * EMMC_BLK);
		wr(R_BLK, (blocks << 16) | (7u << 12) | EMMC_BLK);
		tm = TM_DMA | TM_BLKCNT | (read ? TM_READ : 0) | (blocks > 1 ? TM_MULTI | TM_ACMD12 : 0);
		cmd |= C_DATA;
	}
	wr(R_ARG, arg);
	wr(R_XFER_CMD, cmd << 16 | tm);
	emmc_info.err_cmd = idx;
	return 0;
}

/*
 * Poll the command started by cmd_issue. 1 = still running, 0 = done, <0 = error (lines reset).
 * want_xfer: also wait for transfer complete (data or R1b busy).
 */
static int cmd_poll(int want_xfer, uint64_t t0, uint32_t limit_us, int check_acmd)
{
	uint32_t st = rd(R_INT);

	if (st & INT_ERR) {
		emmc_info.err_status = st;
		emmc_info.err_auto = rd(R_AUTO_HC2) & 0xffff;
		reset_lines();
		return (st & ERR_CMD_MASK) && !(st & INT_CMD_DONE) ? EMMC_E_CMD : EMMC_E_DATA;
	}
	if (!(st & INT_CMD_DONE) || (want_xfer && !(st & INT_XFER_DONE))) {
		if (now_us() - t0 > limit_us) {
			emmc_info.err_status = st;
			reset_lines();
			return EMMC_E_TIMEOUT;
		}
		return 1;
	}
	uint32_t r1 = rd(R_RESP0);
	wr(R_INT, st);
	if ((r1 & R1_ERRORS) || R1_STATE(r1) != R1_TRAN) {
		emmc_info.err_r1 = r1;
		return EMMC_E_CARD;
	}
	if (check_acmd) {
		uint32_t r12 = rd(R_RESP3);
		if (r12 & R1_ERRORS) {
			emmc_info.err_r1 = r12;
			return EMMC_E_CARD;
		}
	}
	return 0;
}

static int cmd_wait(int want_xfer, uint32_t limit_us, int check_acmd)
{
	uint64_t t0 = now_us();
	int rc;
	while ((rc = cmd_poll(want_xfer, t0, limit_us, check_acmd)) == 1)
		if (idle_fn)
			idle_fn();
	return rc;
}

int emmc_init(void)
{
	int rc;

	if (inited)
		return 0;
	struct emmc_info *i = &emmc_info;
	/* MMIO to an unclocked host can stall the bus: look at the GCC branch clocks first */
	i->gcc_ahb = rd_at(GCC_BASE + GCC_SDCC1_AHB);
	i->gcc_apps = rd_at(GCC_BASE + GCC_SDCC1_APPS);
	if ((i->gcc_ahb | i->gcc_apps) & CBCR_OFF)
		return i->rc = EMMC_E_NOTREADY;
	i->host_ver = rd(R_VERSION) >> 16;
	i->caps = rd(R_CAPS);
	i->clk = rd(R_CLK) & 0xffff;
	i->hc1 = rd(R_HC1);
	i->hc2 = rd(R_AUTO_HC2) >> 16;
	i->cqcfg = rd(CQ_CFG);
	i->pstate = rd(R_PSTATE);
	if ((i->host_ver & 0xff) < 2 || !(i->caps & CAPS_ADMA2) || (i->hc2 & HC2_V4_EN))
		return i->rc = EMMC_E_NOTREADY;
	if ((i->clk & (CLK_INT_STABLE | CLK_SD_EN)) != (CLK_INT_STABLE | CLK_SD_EN) || !(i->hc1 & PWR_ON))
		return i->rc = EMMC_E_NOTREADY;
	if (i->cqcfg & 1)
		return i->rc = EMMC_E_NOTREADY;	/* command queueing engine owns the card */
	if ((rc = wait_idle(PS_CMD_INH | PS_DAT_INH)))
		return i->rc = rc;

	/* our side: ADMA2 32-bit, maximum hardware timeout, polled status, no interrupt signals */
	wr8(R_HC1, (uint8_t)((i->hc1 & ~HC1_DMA_MASK) | HC1_ADMA2_32));
	wr8(R_TOUT, 0x0e);
	wr(R_SIG_EN, 0);
	wr(R_INT_EN, INT_EN_VAL);
	wr(R_INT, 0xffffffffu);

	/* the card: SEND_EXT_CSD answers only in the transfer state */
	if ((rc = cmd_issue(8, 0, C_RESP_R1, ext_csd, 1, 1)) || (rc = cmd_wait(1, T_CMD_US, 0)))
		return i->rc = rc;
	dc_inval(ext_csd, sizeof ext_csd);
	i->sectors = (uint64_t)ext_csd[212] | (uint64_t)ext_csd[213] << 8 | (uint64_t)ext_csd[214] << 16 |
		     (uint64_t)ext_csd[215] << 24;
	i->ext_csd_rev = ext_csd[192];
	i->part_config = ext_csd[179];
	i->bus_width = ext_csd[183];
	i->hs_timing = ext_csd[185];
	i->cache_ctrl = ext_csd[33];
	i->cache_size_mb = (uint8_t)((ext_csd[249] | ext_csd[250] << 8 | ext_csd[251] << 16 | (uint32_t)ext_csd[252] << 24) / 1024);
	if (i->part_config & 7)
		return i->rc = EMMC_E_PART;
	if (i->sectors < (4u << 20))		/* <= 2 GB would be byte addressed */
		return i->rc = EMMC_E_NOTREADY;
	inited = 1;
	return i->rc = 0;
}

/* ---- transfers ---- */

static int x_issue(void)
{
	uint32_t n = x.left > MAX_BLKS ? MAX_BLKS : (uint32_t)x.left;
	unsigned idx = x.write ? (n > 1 ? 25 : 24) : (n > 1 ? 18 : 17);
	int rc = cmd_issue(idx, (uint32_t)x.lba, C_RESP_R1, x.buf, n, !x.write);

	if (rc)
		return rc;
	x.cbuf = x.buf;
	x.cblks = n;
	x.lba += n;
	x.left -= n;
	x.buf += (uint64_t)n * EMMC_BLK;
	x.cmd_out = 1;
	x.t0 = now_us();
	return 0;
}

static int x_start(uint64_t lba, uint64_t nblk, void *buf, int write)
{
	if (!inited)
		return EMMC_E_NOTREADY;
	if (x.active || !nblk || ((uintptr_t)buf & 63) || lba + nblk > emmc_info.sectors || lba + nblk > 0xffffffffULL)
		return EMMC_E_ARGS;
	if (dma_addr(buf) + nblk * EMMC_BLK > (1ULL << 32))
		return EMMC_E_ARGS;
	x.active = 1;
	x.write = write;
	x.lba = lba;
	x.left = nblk;
	x.buf = buf;
	int rc = x_issue();
	if (rc)
		x.active = 0;
	return rc;
}

/* Finish the transfer: every command in turn. */
static int x_wait(void)
{
	int rc = 0;

	if (!x.active)
		return EMMC_E_ARGS;
	while (x.cmd_out) {
		rc = cmd_wait(1, T_DATA_US, x.cblks > 1);
		x.cmd_out = 0;
		if (rc)
			break;
		if (!x.write)
			dc_inval(x.cbuf, (uint64_t)x.cblks * EMMC_BLK);
		if (x.write)
			emmc_blocks_written += x.cblks;
		else
			emmc_blocks_read += x.cblks;
		if (x.left && (rc = x_issue()))
			break;
	}
	x.active = 0;
	x.cmd_out = 0;
	return rc;
}

int emmc_read_start(uint64_t lba, uint64_t nblk, void *dst)
{
	return x_start(lba, nblk, dst, 0);
}

int emmc_read_wait(void)
{
	return x_wait();
}

int emmc_read(uint64_t lba, uint64_t nblk, void *dst)
{
	int rc = x_start(lba, nblk, dst, 0);
	return rc ? rc : x_wait();
}

int emmc_write(uint64_t lba, uint64_t nblk, const void *src)
{
#if EMMC_WRITE
	int rc = x_start(lba, nblk, (void *)(uintptr_t)src, 1);
	return rc ? rc : x_wait();
#else
	(void)lba;
	(void)nblk;
	(void)src;
	return EMMC_E_RO;
#endif
}

int emmc_sync(void)
{
	int rc;

	if (!inited)
		return EMMC_E_NOTREADY;
	if (!(emmc_info.cache_ctrl & 1))
		return 0;
	/* CMD6 SWITCH, access 3 (write byte), index 32 FLUSH_CACHE, value 1; R1b */
	if ((rc = cmd_issue(6, 3u << 24 | 32u << 16 | 1u << 8, C_RESP_R1B, 0, 0, 0)))
		return rc;
	return cmd_wait(1, T_DATA_US, 0);
}

/* ---- blkdev adapter for storage.c: LUN 0 = the user data area ---- */

static const char *rc_str(int rc)
{
	return rc ? emmc_errstr(rc) : 0;
}

static const char *bd_cap(void *ctx, unsigned lun, uint64_t *nblk)
{
	(void)ctx;
	if (lun)
		return "no such LUN (eMMC user area only)";
	*nblk = emmc_info.sectors;
	return inited ? 0 : emmc_errstr(EMMC_E_NOTREADY);
}

static const char *bd_read(void *ctx, unsigned lun, uint64_t lba, uint64_t nblk, void *dst)
{
	(void)ctx;
	return lun ? "no such LUN" : rc_str(emmc_read(lba, nblk, dst));
}

static const char *bd_read_start(void *ctx, unsigned lun, uint64_t lba, uint64_t nblk, void *dst)
{
	(void)ctx;
	return lun ? "no such LUN" : rc_str(emmc_read_start(lba, nblk, dst));
}

static const char *bd_read_wait(void *ctx)
{
	(void)ctx;
	return rc_str(emmc_read_wait());
}

static const char *bd_write(void *ctx, unsigned lun, uint64_t lba, uint64_t nblk, const void *src)
{
	(void)ctx;
	return lun ? "no such LUN" : rc_str(emmc_write(lba, nblk, src));
}

static const char *bd_unmap(void *ctx, unsigned lun, uint64_t lba, uint64_t nblk)
{
	(void)ctx;
	(void)lun;
	(void)lba;
	(void)nblk;
	return "eMMC discard not implemented";
}

static const char *bd_sync(void *ctx, unsigned lun)
{
	(void)ctx;
	return lun ? "no such LUN" : rc_str(emmc_sync());
}

void emmc_blkdev(struct blkdev *bd)
{
	bd->ctx = 0;
	bd->blksz = EMMC_BLK;
	bd->cap = bd_cap;
	bd->read = bd_read;
	bd->read_start = bd_read_start;
	bd->read_wait = bd_read_wait;
	bd->write = bd_write;
	bd->unmap = bd_unmap;
	bd->sync = bd_sync;
}

#ifndef EMMC_HOST_TEST
/* ---- read-only probe for the diagnostics screen (enxerto.c, no payload): photo it ---- */
#include "console.h"

static uint8_t probe[34 * EMMC_BLK] __attribute__((aligned(64)));

static uint64_t le64p(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

void test_emmc(void)
{
	con_str("EMMC", "probe (read-only)");
	int rc = emmc_init();
	struct emmc_info *i = &emmc_info;
	con_hex2("EMMC GCC", "ahb=", i->gcc_ahb, "apps=", i->gcc_apps);
	con_hex2("EMMC HOST", "ver=", i->host_ver, "caps=", i->caps);
	con_hex2("EMMC HOST", "clk=", i->clk, "hc1=", i->hc1);
	con_hex2("EMMC HOST", "hc2=", i->hc2, "cq=", i->cqcfg);
	con_hex2("EMMC INIT", "rc=", (uint32_t)rc, "st=", i->err_status);
	if (rc) {
		con_str("EMMC", emmc_errstr(rc));
		con_hex2("EMMC ERR", "r1=", i->err_r1, "acmd=", i->err_auto);
		return;
	}
	con_hex2("EMMC CARD", "sectors=", (uint32_t)i->sectors, "rev=", i->ext_csd_rev);
	con_hex2("EMMC CARD", "part=", i->part_config, "hs=", i->hs_timing << 8 | i->bus_width);
	con_hex2("EMMC CARD", "cache=", i->cache_ctrl, "MiB=", i->cache_size_mb);
	rc = emmc_read(0, 34, probe);
	con_hex2("EMMC READ", "rc=", (uint32_t)rc, "r1=", i->err_r1);
	if (rc)
		return;
	const uint8_t *gpt = probe + EMMC_BLK;
	char sig[9];
	for (int k = 0; k < 8; k++)
		sig[k] = (gpt[k] >= 32 && gpt[k] < 127) ? (char)gpt[k] : '.';
	sig[8] = 0;
	con_hex2("EMMC MBR", "sig=", probe[510] | probe[511] << 8, "type=", probe[450]);
	con_str("EMMC GPT", sig);
	uint32_t nent = gpt[80] | gpt[81] << 8, esz = gpt[84] | gpt[85] << 8;
	con_hex2("EMMC GPT", "entries=", nent, "lba=", (uint32_t)le64p(gpt + 72));
	if (esz != 128 || nent > 128 || le64p(gpt + 72) != 2)
		return;
	/* entries LBA 2..33 are already in probe: the big partitions (>= 64 MiB) */
	for (uint32_t k = 0; k < nent; k++) {
		const uint8_t *e = probe + 2 * EMMC_BLK + k * 128;
		uint64_t first = le64p(e + 32), last = le64p(e + 40);
		if (!first || ((last - first + 1) >> 11) < 64)
			continue;
		char name[17];
		for (int c = 0; c < 16; c++) {
			uint8_t ch = e[56 + 2 * c];
			name[c] = (ch >= 32 && ch < 127) ? (char)ch : (ch ? '?' : 0);
		}
		name[16] = 0;
		con_hex2(name, "lba=", (uint32_t)first, "MiB=", (uint32_t)((last - first + 1) >> 11));
	}
}
#endif
