/*
 * Host (macOS arm64) versions of the kernel's LSE emulation primitives
 * (xnu/patches-12377/1020-sweet-lse-emulation.patch, machine_routines_asm.s). The loops are
 * the kernel's own macros from osfmk/arm64/sweet_lse_asm.h (check.py passes -I to the
 * header extracted from the patch); only the copyio prolog/recovery is left out.
 */
#include "sweet_lse_asm.h"

/* int host_cas(void *addr, uint64_t expected, uint64_t desired, uint64_t *old, uint32_t size) */
	.text
	.align 2
	.globl _host_cas
_host_cas:
	SWEET_LSE_CAS_BODY Lhost_cas_done, Lhost_cas_busy
Lhost_cas_done:
	dmb	ish
	str	x5, [x3]
	mov	x0, #0
	ret
Lhost_cas_busy:
	dmb	ish
	mov	x0, #35		/* EAGAIN */
	ret

/* int host_cas128(void *addr, uint64_t exp_lo, uint64_t exp_hi, uint64_t des_lo, uint64_t des_hi, uint64_t old[2]) */
	.globl _host_cas128
	.align 2
_host_cas128:
	SWEET_LSE_CAS128_BODY Lhost_cas128_done, Lhost_cas128_busy
Lhost_cas128_done:
	dmb	ish
	stp	x10, x11, [x5]
	mov	x0, #0
	ret
Lhost_cas128_busy:
	dmb	ish
	mov	x0, #35
	ret

/* int host_load(void *addr, uint64_t *val, uint32_t size) */
	.globl _host_load
	.align 2
_host_load:
	mov	x3, x1
	mov	w4, w2
	SWEET_LSE_LOAD_BODY
	str	x5, [x3]
	mov	x0, #0
	ret
