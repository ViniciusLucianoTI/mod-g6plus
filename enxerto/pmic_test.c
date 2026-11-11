/*
 * Host test of pmic.c against a simulated SPMI PMIC arbiter in the board's layout (v5 for beryllium,
 * v2/v3 for evert): APID map, ownership table, observer read channels and write channels at the
 * strides the board's arbiter uses. Any access outside the modelled registers, a wrong command format
 * or a bus access for a register outside pmic.c's allowlists fails the test.
 *   cc -DPMIC_HOST_TEST -DBOARD_EVERT     pmic.c pmic_test.c
 *   cc -DPMIC_HOST_TEST -DBOARD_BERYLLIUM pmic.c pmic_test.c
 */
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "pmic.h"

#if defined(SPMI_ARB_V3)
#define VER	0x30010000u
#define MAP	0x800u
#define OBS_ST	0x8000UL
#define CH_ST	0x8000UL
#else
#define VER	0x50020000u
#define MAP	0x900u
#define OBS_ST	0x80UL
#define CH_ST	0x10000UL
#endif
#define NAPID	512

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint16_t apid_ppid[NAPID];	/* 0 = unmapped */
static uint8_t apid_owner[NAPID];
static uint8_t regs[16][0x10000];	/* [sid][addr] */
static uint32_t obs_cmd[NAPID], ch_wdata[NAPID], ch_cmd[NAPID];
static unsigned bus_reads, bus_writes;
static int last_rw_sid = -1, last_rw_addr = -1;

static int apid_of(uintptr_t a, uintptr_t base, uintptr_t stride, uintptr_t *reg)
{
	if (a < base || a >= base + stride * NAPID)
		return -1;
	*reg = (a - base) % stride;
	return (int)((a - base) / stride);
}

uint32_t pmic_test_rd(uintptr_t a)
{
	uintptr_t reg;
	int ap;

	if (a == SPMI_CORE)
		return VER;
#if !defined(SPMI_ARB_V3)
	if (a == SPMI_CORE + 4)
		return NAPID;
#endif
	if (a >= SPMI_CORE + MAP && a < SPMI_CORE + MAP + 4 * NAPID) {
		unsigned i = (unsigned)(a - SPMI_CORE - MAP) / 4;
		return apid_ppid[i] ? (uint32_t)apid_ppid[i] << 8 : 0;
	}
	if (a >= SPMI_CNFG + 0x700 && a < SPMI_CNFG + 0x700 + 4 * NAPID)
		return apid_owner[(a - SPMI_CNFG - 0x700) / 4];
	if ((ap = apid_of(a, SPMI_OBSRVR, OBS_ST, &reg)) >= 0) {
		if (reg == 0x08)
			return 1;	/* done */
		if (reg == 0x18) {
			unsigned sid = apid_ppid[ap] >> 8, addr = (apid_ppid[ap] & 0xff) << 8 | ((obs_cmd[ap] >> 4) & 0xff);
			bus_reads++;
			last_rw_sid = (int)sid;
			last_rw_addr = (int)addr;
			return regs[sid][addr];
		}
	}
	if ((ap = apid_of(a, SPMI_CHNLS, CH_ST, &reg)) >= 0 && reg == 0x08)
		return 1;
	CHECK(0, "read of unmodelled address %#lx", (unsigned long)a);
	return 0xdeadbeef;
}

void pmic_test_wr(uintptr_t a, uint32_t v)
{
	uintptr_t reg;
	int ap;

	if ((ap = apid_of(a, SPMI_OBSRVR, OBS_ST, &reg)) >= 0 && reg == 0) {
		CHECK(apid_ppid[ap], "observer command on unmapped apid %d", ap);
#if defined(SPMI_ARB_V3)
		CHECK(ap != 511, "observer command on the reserved channel");
#endif
		CHECK((v >> 27) == 1 && (v & 7) == 0 && !(v & 0x07fff000), "observer cmd %#x not EXT_READL 1 byte", v);
		obs_cmd[ap] = v;
		return;
	}
	if ((ap = apid_of(a, SPMI_CHNLS, CH_ST, &reg)) >= 0) {
		if (reg == 0x10) {
			ch_wdata[ap] = v;
			return;
		}
		if (reg == 0) {
			unsigned sid = apid_ppid[ap] >> 8, base = (apid_ppid[ap] & 0xff) << 8, off = (v >> 4) & 0xff, bc = (v & 7) + 1;
			CHECK(apid_ppid[ap], "write command on unmapped apid %d", ap);
#if defined(SPMI_ARB_V3)
			CHECK(ap != 511, "write command on the reserved channel");
#endif
			CHECK((v >> 27) == 0, "write cmd %#x not EXT_WRITEL", v);
			CHECK(apid_owner[ap] == 0, "write through an apid owned by EE %d", apid_owner[ap]);
			for (unsigned i = 0; i < bc; i++)
				regs[sid][base + off + i] = (uint8_t)(ch_wdata[ap] >> (8 * i));
			ch_cmd[ap] = v;
			bus_writes++;
			return;
		}
	}
	CHECK(0, "write %#x to unmodelled address %#lx", v, (unsigned long)a);
}

static void map(unsigned apid, unsigned sid, unsigned periph, unsigned owner)
{
	apid_ppid[apid] = (uint16_t)(sid << 8 | periph);
	apid_owner[apid] = (uint8_t)owner;
}

int main(void)
{
	printf("pmic_test: %s, arbiter %s, PON sid %d, charger/FG sid %d, WLED sid %d\n", BOARD_NAME,
#if defined(SPMI_ARB_V3)
	       "v3",
#else
	       "v5",
#endif
	       PMIC_SID_PON, PMIC_SID_CHG, PMIC_SID_WLED);
	/* the peripherals pmic.c may touch, at scattered APIDs (high ones exercise the stride) */
	map(3, PMIC_SID_PON, 0x08, 0);
	map(40, PMIC_SID_CHG, 0x10, 0);
	map(41, PMIC_SID_CHG, 0x13, 0);
	map(42, PMIC_SID_CHG, 0x16, 0);
	map(200, PMIC_SID_CHG, 0x40, 0);
	map(201, PMIC_SID_CHG, 0x41, 0);
	map(300, PMIC_SID_WLED, 0xd8, 0);
	map(510, PMIC_SID_WLED, 0xd9, 0);
	map(7, PMIC_SID_CHG, 0x12, 0);		/* mapped but not on any allowlist */
#if defined(SPMI_ARB_V3)
	map(511, PMIC_SID_CHG, 0x10, 0);	/* the reserved channel duplicates CHGR: must never be used */
#endif

	CHECK(pmic_init() == 0, "pmic_init");
	CHECK(pmic_arb_version() == VER, "version");

	/* keys: PON RT_STS bit0 power, bit1 resin (volume down) */
	regs[PMIC_SID_PON][0x810] = 0x2;
	CHECK(pmic_keys() == PMIC_KEY_VOLDN, "keys %#x", pmic_keys());
	regs[PMIC_SID_PON][0x808] = 0x80;
	CHECK(pmic_pon_reason() == 0x80, "pon reason");

	/* battery: USB in, fast charge, SOC raw 128, 3.9 V, -500 mA FG (= charging 500) */
	regs[PMIC_SID_CHG][0x1310] = 0x10;
	regs[PMIC_SID_CHG][0x1006] = 3;
	regs[PMIC_SID_CHG][0x1308] = 0x08;
	regs[PMIC_SID_CHG][0x4009] = 128;
	unsigned rv = 3900u * 1000000u / 122070u;
	regs[PMIC_SID_CHG][0x41a0] = rv & 0xff;
	regs[PMIC_SID_CHG][0x41a1] = rv >> 8;
	int16_t rc = (int16_t)(-(500 * 1000000 / 488281));
	regs[PMIC_SID_CHG][0x41a2] = (uint16_t)rc & 0xff;
	regs[PMIC_SID_CHG][0x41a3] = (uint16_t)rc >> 8;
	struct pmic_batt b;
	pmic_read_batt(&b);
	CHECK(b.valid && b.usb_present && b.chg == PMIC_CHG_CHARGING && b.apsd == 8, "batt flags");
	CHECK(b.mv >= 3898 && b.mv <= 3900, "mv %u", b.mv);
	CHECK(b.soc == 51, "soc %u", b.soc);	/* (127 * 98 + 252) / 253 + 1 */
	CHECK(b.ma_valid && b.ma >= 495 && b.ma <= 500, "ma %d", b.ma);

	/* WLED: set, latched, read back */
	uint16_t v[2] = { 0x123, 0x456 }, g[2];
	CHECK(pmic_wled_set(v) == 0, "wled set");
	CHECK(pmic_wled_get(g) == 0 && g[0] == 0x123 && g[1] == 0x456, "wled get");
	CHECK(regs[PMIC_SID_WLED][0xd947] == 0x0f, "wled sync latch");
	regs[PMIC_SID_WLED][0xd846] = 0x80;
	CHECK(pmic_wled_enabled() == 1, "wled enabled");

	/* charger: an allowlisted register is written and verified, others never reach the bus */
	CHECK(pmic_chg_write(0x1370, 0x28) == 0 && regs[PMIC_SID_CHG][0x1370] == 0x28, "chg write allowed");
	unsigned w0 = bus_writes, r0 = bus_reads;
	CHECK(pmic_chg_write(0x1200, 0x01) == -1, "chg write outside allowlist");
	CHECK(pmic_chg_write(0x1062, 0x01) == -1, "chg write neighbour register");
	uint8_t x;
	CHECK(pmic_chg_read(0x1200, &x) == -1, "read outside allowlist");
	CHECK(bus_writes == w0 && bus_reads == r0, "refused accesses reached the bus");
	/* a peripheral another EE owns is never written */
	apid_owner[40] = 2;	/* CHGR 0x10 */
	regs[PMIC_SID_CHG][0x1042] = 0;
	CHECK(pmic_chg_write(0x1042, 0x01) == -1 && regs[PMIC_SID_CHG][0x1042] == 0, "foreign-owned write");
	apid_owner[40] = 0;
	/* PON shares SID 0 with the charger on SDM630: its block stays read-only */
	CHECK(pmic_chg_write(0x0810, 0x00) == -1, "PON write");
#if !defined(SPMI_ARB_V3)
	/* PON is only readable on the PON SID (PMI8998 SID 2 has no business there) */
	map(9, PMIC_SID_CHG, 0x08, 0);
	r0 = bus_reads;
	CHECK(pmic_chg_read(0x0808, &x) == -1 && bus_reads == r0, "PON block through the charger SID");
#endif
	printf(fails ? "pmic_test: %d FAILURES\n" : "pmic_test: all passed\n", fails);
	return fails != 0;
}
