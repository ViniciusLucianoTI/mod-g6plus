/*
 * Host test of the kernel LSE emulation (xnu/patches-12377/1020-sweet-lse-emulation.patch:
 * osfmk/arm64/sweet_lseemu.h + the LDXR/STXR loops of sweet_lse_asm.h). Run through check.py,
 * which extracts the headers from the patch and generates forms_gen.S / forms_gen.c (one
 * function per LSE form, executed natively on the Mac's LSE hardware).
 *
 *   lseemu_test unit             every form: emulated vs native on random inputs; concurrency
 *                                (emulated and native atomics on the same words); misalignment
 *   lseemu_test decode < words   one hex word per line -> "WORD canonical"
 *   lseemu_test bench            ns per emulated atomic (decode + execute + LL/SC), vs native
 */
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sweet_lseemu.h"

/* lse_host.S: the kernel's loops without the copyio wrapper */
extern int host_cas(void *addr, uint64_t expected, uint64_t desired, uint64_t *old, uint32_t size);
extern int host_cas128(void *addr, uint64_t exp_lo, uint64_t exp_hi, uint64_t des_lo, uint64_t des_hi,
    uint64_t old[2]);
extern int host_load(void *addr, uint64_t *val, uint32_t size);

/* forms_gen.S/.c: void form(void *mem, uint64_t r[4]): x2,x3 = r[0],r[1] (Rs pair), x4,x5 = r[2],r[3] (Rt pair) */
typedef void (*form_fn)(void *mem, uint64_t r[4]);
struct form {
	form_fn fn;
	const char *text;
};
extern const struct form forms[];
extern const unsigned nforms;

static unsigned fails;
#define FAIL(...) do { if (++fails <= 40) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- host memory callbacks: the kernel's copy_validate alignment rule, then the asm loops ---- */
static int
h_load(void *ctx, uint64_t addr, unsigned size, uint64_t *val)
{
	(void)ctx;
	if (addr & ((1ull << size) - 1)) {
		return EINVAL;
	}
	return host_load((void *)addr, val, size);
}

static int
h_cas(void *ctx, uint64_t addr, unsigned size, uint64_t expected, uint64_t desired, uint64_t *old)
{
	(void)ctx;
	if (addr & ((1ull << size) - 1)) {
		return EINVAL;
	}
	return host_cas((void *)addr, expected, desired, old, size);
}

static int
h_cas128(void *ctx, uint64_t addr, uint64_t elo, uint64_t ehi, uint64_t dlo, uint64_t dhi, uint64_t *olo, uint64_t *ohi)
{
	uint64_t old[2] = {0, 0};
	(void)ctx;
	if (addr & 15) {
		return EINVAL;
	}
	int e = host_cas128((void *)addr, elo, ehi, dlo, dhi, old);
	*olo = old[0];
	*ohi = old[1];
	return e;
}

static const struct sweet_lse_mem hmem = { .load = h_load, .cas = h_cas, .cas128 = h_cas128, .ctx = NULL };

static uint32_t
form_word(const struct form *f)
{
	uint32_t w;
	memcpy(&w, (const char *)f->fn + 8, 4);     /* ldp x2,x3 ; ldp x4,x5 ; INSN */
	return w;
}

static const struct form *
find_form(const char *text)
{
	for (unsigned i = 0; i < nforms; i++) {
		if (strcmp(forms[i].text, text) == 0) {
			return &forms[i];
		}
	}
	fprintf(stderr, "no form '%s'\n", text);
	exit(2);
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t
rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

/* interesting values: small, sign boundaries of every size, random */
static uint64_t
rval(void)
{
	static const uint64_t edge[] = {
		0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
		0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, ~0ull, 0xFFFFFFFF00000000ull,
	};
	switch (rnd() % 4) {
	case 0: return edge[rnd() % (sizeof(edge) / sizeof(edge[0]))];
	case 1: return rnd() & 0xFF;
	default: return rnd();
	}
}

/* emulate word on a cpu whose x0 = mem, x2..x5 = r[] */
static enum sweet_lse_result
emulate(uint32_t w, void *mem, uint64_t r[4], uint32_t *written, int *err)
{
	struct sweet_lse_cpu cpu;
	uint64_t fa = 0;
	for (unsigned i = 0; i < 31; i++) {
		cpu.x[i] = 0x5EED000000000000ull | i;
	}
	cpu.x[0] = (uint64_t)mem;
	cpu.x[1] = 0x1111;
	memcpy(&cpu.x[2], r, 4 * sizeof(uint64_t));
	cpu.sp = 0x5EEDull << 32;
	cpu.pc = 0x100000;
	enum sweet_lse_result res = sweet_lse_execute(w, &cpu, &hmem, err, &fa);
	if (res == SWEET_LSE_DONE) {
		if (cpu.pc != 0x100004) {
			FAIL("%08x: pc 0x%" PRIx64, w, cpu.pc);
		}
		for (unsigned i = 0; i < 31; i++) {
			bool expect_same = !(cpu.written & (1u << i));
			if (expect_same && i >= 2 && i <= 5) {
				continue;       /* compared against native below */
			}
			if (expect_same && cpu.x[i] != (i == 0 ? (uint64_t)mem : i == 1 ? 0x1111 : (0x5EED000000000000ull | i))) {
				FAIL("%08x: x%u changed but not marked written", w, i);
			}
			if (!expect_same && (i < 2 || i > 5)) {
				FAIL("%08x: wrote x%u", w, i);
			}
		}
		memcpy(r, &cpu.x[2], 4 * sizeof(uint64_t));
		*written = cpu.written;
	}
	return res;
}

static void
test_forms(void)
{
	static _Alignas(16) uint8_t mem_n[32], mem_e[32];
	unsigned runs = 0;

	for (unsigned fi = 0; fi < nforms; fi++) {
		const struct form *f = &forms[fi];
		const uint32_t w = form_word(f);
		struct sweet_lse_insn in;
		if (!sweet_lse_decode(w, &in)) {
			FAIL("%s (%08x) not decoded", f->text, w);
			continue;
		}
		for (unsigned t = 0; t < 3000; t++) {
			uint64_t rn[4], re[4], m[4];
			for (unsigned i = 0; i < 4; i++) {
				rn[i] = rval();
				m[i] = rval();
			}
			if (t & 1) {
				/* make CAS/CASP compare equal (also exercises the store path of everything) */
				m[0] = rn[0];
				if (in.op == SWEET_LSE_CASP && in.size == 2) {
					m[0] = (rn[0] & 0xFFFFFFFF) | rn[1] << 32;
				} else if (in.op == SWEET_LSE_CASP) {
					m[1] = rn[1];
				}
			}
			memcpy(re, rn, sizeof(rn));
			memcpy(mem_n, m, 32);
			memcpy(mem_e, m, 32);
			f->fn(mem_n, rn);
			uint32_t written = 0;
			int err = 0;
			if (emulate(w, mem_e, re, &written, &err) != SWEET_LSE_DONE) {
				FAIL("%s: emulation failed err=%d", f->text, err);
				break;
			}
			runs++;
			if (memcmp(rn, re, sizeof(rn)) || memcmp(mem_n, mem_e, 32)) {
				FAIL("%s (%08x): native r=%016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64
				    " m=%016" PRIx64 " %016" PRIx64 " | emulated r=%016" PRIx64 " %016" PRIx64 " %016" PRIx64
				    " %016" PRIx64 " m=%016" PRIx64 " %016" PRIx64 " | in r=%016" PRIx64 " %016" PRIx64
				    " m=%016" PRIx64 " %016" PRIx64, f->text, w,
				    rn[0], rn[1], rn[2], rn[3], ((uint64_t *)mem_n)[0], ((uint64_t *)mem_n)[1],
				    re[0], re[1], re[2], re[3], ((uint64_t *)mem_e)[0], ((uint64_t *)mem_e)[1],
				    m[0], m[1], m[0], m[1]);
				break;
			}
		}
	}
	printf("forms: %u forms, %u random cases emulated == native\n", nforms, runs);
}

static void
test_misaligned(void)
{
	static _Alignas(16) uint8_t mem[64];
	unsigned n = 0;
	for (unsigned fi = 0; fi < nforms; fi++) {
		const uint32_t w = form_word(&forms[fi]);
		struct sweet_lse_insn in;
		sweet_lse_decode(w, &in);
		const unsigned bytes = sweet_lse_access_bytes(&in);
		if (bytes == 1) {
			continue;
		}
		uint64_t r[4] = {1, 2, 3, 4}, r0[4];
		memcpy(r0, r, sizeof(r));
		memset(mem, 0xA5, sizeof(mem));
		uint32_t written = 0;
		int err = 0;
		if (emulate(w, mem + bytes / 2, r, &written, &err) != SWEET_LSE_FAULT || err != EINVAL) {
			FAIL("%s: misaligned access not refused (err %d)", forms[fi].text, err);
		}
		for (unsigned i = 0; i < sizeof(mem); i++) {
			if (mem[i] != 0xA5) {
				FAIL("%s: misaligned access wrote memory", forms[fi].text);
				break;
			}
		}
		if (memcmp(r, r0, sizeof(r))) {
			FAIL("%s: misaligned access changed registers", forms[fi].text);
		}
		n++;
	}
	printf("misaligned: %u forms refused with EINVAL, nothing written\n", n);
}

/* ---- concurrency: emulated and native atomics on the same words ---- */
#define THREADS 8
#define ITERS   100000

struct conc {
	const char *native, *emul;
	void *mem;
	unsigned kind;          /* 0: +1 counter (Rs = 1), 1: CASP X pair loop, 2: SWP lock + plain counter */
	volatile uint64_t *plain;
};

static void
emul_or_native(const struct conc *c, bool emul, uint64_t r[4])
{
	if (!emul) {
		find_form(c->native)->fn(c->mem, r);
		return;
	}
	uint32_t written = 0;
	int err = 0;
	if (emulate(form_word(find_form(c->emul)), c->mem, r, &written, &err) != SWEET_LSE_DONE) {
		FAIL("%s: emulation failed in a thread", c->emul);
	}
}

struct targ {
	const struct conc *c;
	bool emul;
};

static void *
conc_thread(void *p)
{
	const struct targ *a = p;
	const struct conc *c = a->c;
	for (unsigned i = 0; i < ITERS; i++) {
		if (c->kind == 0) {
			uint64_t r[4] = {1, 0, 0, 0};
			emul_or_native(c, a->emul, r);
		} else if (c->kind == 1) {
			/* 128-bit counter: lo and hi both +1, by a CASP loop */
			uint64_t *m = c->mem;
			uint64_t lo = m[0], hi = m[1];
			for (;;) {
				uint64_t r[4] = {lo, hi, lo + 1, hi + 1};
				emul_or_native(c, a->emul, r);
				if (r[0] == lo && r[1] == hi) {
					break;
				}
				lo = r[0];
				hi = r[1];
			}
		} else {
			/* spinlock: SWPA 1 until 0 came back; plain ++; release with a plain STLR */
			for (;;) {
				uint64_t r[4] = {1, 0, 0, 0};
				emul_or_native(c, a->emul, r);
				if (r[2] == 0) {
					break;
				}
			}
			*c->plain = *c->plain + 1;
			__atomic_store_n((uint32_t *)c->mem, 0, __ATOMIC_RELEASE);
		}
	}
	return NULL;
}

static void
run_conc(const struct conc *c, const char *what)
{
	pthread_t th[THREADS];
	struct targ a[THREADS];
	for (unsigned i = 0; i < THREADS; i++) {
		a[i] = (struct targ){ c, (i & 1) != 0 };
		pthread_create(&th[i], NULL, conc_thread, &a[i]);
	}
	for (unsigned i = 0; i < THREADS; i++) {
		pthread_join(th[i], NULL);
	}
	(void)what;
}

static void
test_concurrency(void)
{
	const uint64_t total = (uint64_t)THREADS * ITERS;
	static _Alignas(16) uint64_t m64[2];
	static _Alignas(16) uint8_t bytes[16];
	static _Alignas(16) uint64_t pair[2];
	static _Alignas(16) uint32_t lock;
	static volatile uint64_t plain;

	struct conc c0 = { "ldaddal x2, x4, [x0]", "ldaddal x2, x4, [x0]", m64, 0, NULL };
	run_conc(&c0, "ldaddal x");
	if (m64[0] != total || m64[1] != 0) {
		FAIL("concurrent LDADDAL X: %" PRIu64 " != %" PRIu64, m64[0], total);
	}

	/* byte counter next to a byte that only native code touches: no lost or torn neighbours */
	struct conc c1 = { "ldaddalb w2, w4, [x0]", "ldaddalb w2, w4, [x0]", bytes, 0, NULL };
	struct conc c1n = { "ldaddalb w2, w4, [x0]", "ldaddalb w2, w4, [x0]", bytes + 1, 0, NULL };
	pthread_t side;
	struct targ sa = { &c1n, false };
	pthread_create(&side, NULL, conc_thread, &sa);
	run_conc(&c1, "ldaddalb");
	pthread_join(side, NULL);
	if (bytes[0] != (uint8_t)total || bytes[1] != (uint8_t)ITERS || bytes[2] != 0) {
		FAIL("concurrent LDADDALB: %u (want %u), neighbour %u (want %u)", bytes[0], (uint8_t)total, bytes[1],
		    (uint8_t)ITERS);
	}

	struct conc c2 = { "caspal x2, x3, x4, x5, [x0]", "caspal x2, x3, x4, x5, [x0]", pair, 1, NULL };
	run_conc(&c2, "caspal x");
	if (pair[0] != total || pair[1] != total) {
		FAIL("concurrent CASPAL X: %" PRIu64 ":%" PRIu64 " != %" PRIu64, pair[0], pair[1], total);
	}

	struct conc c3 = { "swpa w2, w4, [x0]", "swpa w2, w4, [x0]", &lock, 2, &plain };
	run_conc(&c3, "swpa lock");
	if (plain != total) {
		FAIL("SWPA spinlock (emulated + native, plain STLR release): counter %" PRIu64 " != %" PRIu64,
		    (uint64_t)plain, total);
	}
	printf("concurrency: %u threads x %u, half emulated half native: LDADDAL X, LDADDALB (+ native neighbour),"
	    " CASPAL X, SWPA lock\n", THREADS, ITERS);
}

static void
canonical(uint32_t w, char *buf, size_t n)
{
	static const char *const ops[] = { "NONE", "LDOP", "SWP", "CAS", "CASP", "LDAPR" };
	struct sweet_lse_insn in;
	if (!sweet_lse_decode(w, &in)) {
		snprintf(buf, n, "NONE");
		return;
	}
	snprintf(buf, n, "%s alu=%u size=%u s=%u t=%u n=%u a=%d r=%d", ops[in.op], in.op == SWEET_LSE_LDOP ? in.alu : 0,
	    in.size, in.op == SWEET_LSE_LDAPR ? 31 : in.rs, in.rt, in.rn, in.acquire, in.release);
}

static double
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static void
bench(void)
{
	static _Alignas(16) uint64_t m[2];
	const char *names[] = { "ldaddal x2, x4, [x0]", "casal w2, w4, [x0]", "swpal x2, x4, [x0]", "caspal x2, x3, x4, x5, [x0]" };
	for (unsigned k = 0; k < 4; k++) {
		const struct form *f = find_form(names[k]);
		const uint32_t w = form_word(f);
		const unsigned N = 2000000;
		double t0 = now_ns();
		for (unsigned i = 0; i < N; i++) {
			uint64_t r[4] = {1, 0, 1, 0};
			uint32_t wr;
			int err;
			emulate(w, m, r, &wr, &err);
		}
		double t1 = now_ns();
		for (unsigned i = 0; i < N; i++) {
			uint64_t r[4] = {1, 0, 1, 0};
			f->fn(m, r);
		}
		double t2 = now_ns();
		printf("bench %-28s emulated %.1f ns, native call %.1f ns (software part only; the trap is extra)\n",
		    names[k], (t1 - t0) / N, (t2 - t1) / N);
	}
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s unit|decode|bench\n", argv[0]);
		return 2;
	}
	if (!strcmp(argv[1], "unit")) {
		test_forms();
		test_misaligned();
		test_concurrency();
		printf("%s: %u failures\n", fails ? "FAIL" : "PASS", fails);
		return fails != 0;
	}
	if (!strcmp(argv[1], "decode")) {
		char line[64], buf[128];
		while (fgets(line, sizeof(line), stdin)) {
			uint32_t w = (uint32_t)strtoul(line, NULL, 16);
			canonical(w, buf, sizeof(buf));
			printf("%08x %s\n", w, buf);
		}
		return 0;
	}
	if (!strcmp(argv[1], "bench")) {
		bench();
		return 0;
	}
	return 2;
}
