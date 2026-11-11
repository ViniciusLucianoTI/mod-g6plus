#pragma once
/*
 * eMMC through the SDHCI host the stock ABL booted from (SDM630 sdhc_1, qcom,sdhci-msm-v5 at EMMC_BASE),
 * polled, ADMA2 32-bit. The counterpart of ufs.c for boards with STORAGE_EMMC (board.h).
 *
 * Like ufs.c it TAKES OVER the link as ABL left it: no card re-initialisation, no clock/bus-mode/tuning
 * change (whatever HS200/HS400/DDR setup ABL tuned stays). emmc_init only checks that the host is up
 * (GCC SDCC1 clocks running, SD clock on, bus power on, SDHCI >= 3.0 with ADMA2, no CQE) and that the
 * card answers SEND_EXT_CSD in the transfer state on the user data area (PARTITION_ACCESS 0), and
 * refuses otherwise. Sector (512 byte) addressing: every eMMC above 2 GB.
 *
 * Commands: CMD8 SEND_EXT_CSD, CMD17/18 READ_SINGLE/MULTIPLE_BLOCK, CMD24/25 WRITE_*, Auto-CMD12 after
 * multi-block transfers (R1b busy handled by the host), CMD6 SWITCH FLUSH_CACHE for sync when the card
 * cache is on. No RCA is needed (no CMD13): every completion is the host's transfer-complete.
 *
 * WRITES are compiled out unless EMMC_WRITE=1 (build.sh): until the read path has been checked on the
 * phone, emmc_write returns EMMC_E_RO. Which blocks may be written is decided by storage.c anyway.
 *
 * DMA is NOT cache coherent: reads flush before and invalidate after, writes clean before (cache.h).
 * Buffers must be 64-byte aligned and below 4 GiB (ADMA2 32-bit).
 */
#include <stdint.h>

#define EMMC_BLK	512u

#define EMMC_E_NOTREADY	(-1)	/* host clocks/power/mode not as expected, or card not in tran state */
#define EMMC_E_TIMEOUT	(-2)
#define EMMC_E_CMD	(-3)	/* command-phase error (timeout/CRC/end bit/index) */
#define EMMC_E_DATA	(-4)	/* data-phase error (CRC/end bit/ADMA/auto CMD12) or software data timeout */
#define EMMC_E_CARD	(-5)	/* R1 card status error bits, or not in the transfer state */
#define EMMC_E_ARGS	(-6)	/* misaligned buffer, DMA above 4 GiB, range past the end, busy ... */
#define EMMC_E_RO	(-7)	/* write path compiled out (EMMC_WRITE=0) */
#define EMMC_E_PART	(-8)	/* PARTITION_ACCESS is not the user data area */

struct emmc_info {
	uint32_t gcc_ahb, gcc_apps;	/* GCC SDCC1 CBCRs as found */
	uint32_t host_ver;		/* SDHCI_HOST_VERSION (16 bit) */
	uint32_t caps, pstate, clk, hc1, hc2, cqcfg;
	uint64_t sectors;		/* EXT_CSD SEC_COUNT */
	uint8_t ext_csd_rev, part_config, bus_width, hs_timing, cache_ctrl, cache_size_mb;
	int rc;				/* emmc_init result */
	/* last error */
	uint32_t err_cmd, err_status, err_auto, err_r1;
};
extern struct emmc_info emmc_info;
extern uint64_t emmc_blocks_read, emmc_blocks_written;

/* Check the host + card as ABL left them (idempotent). 0 or EMMC_E_*. */
int emmc_init(void);
const char *emmc_errstr(int rc);
/* Called while waiting (screen refresh, USB keep-alive); may be NULL. */
void emmc_set_idle(void (*fn)(void));

int emmc_read(uint64_t lba, uint64_t nblk, void *dst);
int emmc_write(uint64_t lba, uint64_t nblk, const void *src);
/* Asynchronous read: start (issues the first command), then wait. One transfer at a time. */
int emmc_read_start(uint64_t lba, uint64_t nblk, void *dst);
int emmc_read_wait(void);
/* Flush the card's volatile cache (no-op when EXT_CSD CACHE_CTRL is off). */
int emmc_sync(void);

struct blkdev;
/* Fill a blkdev (storage.h): one LUN (0 = user data area), 512-byte blocks. */
void emmc_blkdev(struct blkdev *bd);

/* Diagnostics screen: host/card registers, MBR/GPT and the big partitions (read-only). */
void test_emmc(void);
