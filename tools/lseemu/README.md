# lseemu: kernel LSE emulation (xnu patch 1020) checks and test tool

The Moto G6 Plus CPU (SDM630, Kryo 260 Silver = Cortex-A53 class) is ARMv8.0. It has no LSE
atomics (ARMv8.1) and no LDAPR (ARMv8.3 RCpc). The iOS userland uses both inline everywhere
(`docs/G6PLUS_HARDWARE.md` section 2).

`xnu/patches-12377/1020-sweet-lse-emulation.patch` makes Heartwood emulate them for 64-bit EL0
threads. It handles:

- LD{ADD,CLR,EOR,SET,SMAX,SMIN,UMAX,UMIN}, SWP, CAS and CASP, in every size and ordering;
- the ST* aliases;
- LDAPR.

There is no boot-arg: the emulation runs only when `ID_AA64ISAR0/1_EL1` say the CPU lacks the
feature, so it is inert on the Poco F1.

## Design

- **Atomicity.** Every memory update is one compare-and-swap primitive. It is an LDXR/STXR (or
  LDXP/STXP) loop on the user address, inside a copyio recovery range, so the VM resolves page
  faults and the loop retries. LD<op> and SWP are a CAS loop in C around that primitive. Emulated
  atomics are therefore atomic across cores, against each other and against native LL/SC code.
- **Ordering.** DMB ISH before and after each primitive, so every emulated atomic is at least as
  strong as its AL form. LDAPR is done with LDAR.
- **Faults.**
  - A misaligned access gets `EXC_BAD_ACCESS` / `EXC_ARM_DA_ALIGN` (SIGBUS).
  - An unmapped address gets SIGSEGV with the EL0 data-abort ESR, as in 1019.
  - Memory that never holds an exclusive reservation (some device or non-cacheable mappings) gives
    up after 2^20 STXR failures. It then raises `EXC_BAD_ACCESS` with a log line.
- **Profile.** The kernel logs `SWEET: emulating <class> ... in <process>[pid] (N so far, t=<mach time>)`
  for the first four traps of each class, then at every power of two from 1024. The counts and
  timestamps give the trap rate per class. That rate decides whether the shared cache has to be
  rewritten instead (`docs/G6PLUS_HARDWARE.md` section 2, option b).

## Files

| File | What |
| --- | --- |
| `check.py` | host checks (Apple-silicon Mac, Command Line Tools are enough). It extracts `sweet_lseemu.h` and `sweet_lse_asm.h` from the patch and generates one function per form (244 forms). Unit: every form runs **natively on the Mac's LSE hardware** and through the emulator plus the kernel's own LL/SC loops, on 3000 random inputs each. Misaligned accesses are refused without side effects. 8 threads mix emulated and native atomics on the same words. Decode: every form, its bit flips and 30k random words from the LSE encoding spaces are compared against `llvm-objdump`. `--bench`: software cost per emulated atomic. `--emit DIR`: sources for `build.sh` |
| `lseemu_test.c` | the C side; modes `unit`, `decode`, `bench` |
| `lse_host.S` | the kernel's primitives without the copyio wrapper, built from the same macros (`sweet_lse_asm.h`) |
| `build.sh` | builds `out/lseemu_test` for the phone (iOS arm64, ad-hoc signed). **Needs full Xcode (iphoneos SDK)** |

```bash
python3 tools/lseemu/check.py --bench
```

On the G6 Plus, with a kernel that has 1020, the "native" forms in `lseemu_test` trap into the
kernel. So `lseemu_test unit` there checks the kernel emulation against the user-space LL/SC
loops, including mixed concurrency, and `lseemu_test bench` shows the cost of one trap.

```bash
tools/lseemu/build.sh
scp -O tools/lseemu/out/lseemu_test root@192.168.7.2:/var/root/
ssh root@192.168.7.2 /var/root/lseemu_test unit
```

## Not handled (stay SIGILL)

- CASP with an odd register;
- LD64B/ST64B*, LDAPUR* (v8.4), RCW*, LSE128;
- SIMD FP16, DotProd and RDM.

These are a separate problem: they need the FP/SIMD register state in the trap.
