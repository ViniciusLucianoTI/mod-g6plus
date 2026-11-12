# Moto G6 Plus (evert): GPU plan, Adreno 508, iOS 16.7 on xnu-8796

Written 2026-11-12, after touch and keys worked. Decision by the user the same day: **Metal on top of the
open OpenGL ES driver** (freedreno a5xx), not Vulkan.

## Facts the plan rests on

* **No open Vulkan driver for a5xx.** Mesa `docs/features.txt` (main, read 2026-11-12): Vulkan = `tu`
  (Turnip, a6xx and later); `freedreno/a5xx+` has GLES 3.1 / GLSL ES 3.1 and GL 3.3. Qualcomm's blob has
  Vulkan on the 508 but needs Android's kernel (kgsl) and libc.
* **What this repo has** (survey of docs/GPU_*.md, METAL_ADRENO630.md, gpu/, the three patch series):
  * Old line `xnu/patches/` (the G6's): only 0103 (A630 power-up test, GMU based, beryllium only) and the
    empty IOAcceleratorES stub 0122. No GPU service, no user client.
  * `patches-11215/` / `patches-12377/` only: 0181 (submit/fence API, page-table pool, GPU address
    allocator), 0182 (`SweetAdreno630` IOService + user client with the Linux drm/msm ABI,
    `xnu/gpu-abi/sweet_adreno_abi.h`), 0185 (4 KiB GPU pages), 0186/0188 (power, governor), 0214/0217
    (IOSurface alias), 0240 (scanout blit).
  * Userland: `gpu/freedreno-sweet/sweet_drm.c` (libdrm stand-in over the user client), `gpu/eagl/`
    (replacement OpenGLES.framework over Mesa 25.0.7 Gallium freedreno through OSMesa, plus a hook that
    makes backboardd's CoreAnimation use its GLES renderer; **addresses are for iOS 18 22H352**).
  * Metal: Chlorophyll (`gpu/metal` submodule, not checked out here). Its first generation translated Metal
    to **Gallium state over freedreno** with an AIR -> NIR shader front end feeding ir3; the current one is
    Vulkan (Turnip) only. hackios did Metal over Gallium on iOS 16.5 (Adreno 642L).
  * No GPU acceleration was ever reached on iOS 16 in this project, and there is no a5xx code.
* **G6 hardware** (stock DT `dtb/evert/rom-3.dtb`, `docs/G6PLUS_HARDWARE.md`): `qcom,kgsl-3d0@5000000`
  (reg 0x05000000+0x40000, chip id 0x05000800, IRQ SPI 300), GPUCC 0x05065000, GDSCs `gdsc_gpu_cx`
  0x05066004 and `gdsc_gpu_gx` 0x05066094 (parent `vdd_gfx`, an RPM regulator), kgsl SMMU v2 0x05040000,
  zap `a508_zap`, firmware `a530_pm4.fw` / `a530_pfp.fw` on the phone. No GMU.
* **Already working on the G6 that the GPU needs:** the RPM client (`erpm_*` in sweet_touch.c, patch 1036),
  SPMI observer reads (1039), root and console over USB, input.

"Metal to OpenGL ES" in practice: the Metal front end drives the **Gallium interface of the same open driver
that implements GLES 3.1**, it does not emit `gl*` calls. Same driver and limits; shaders go AIR -> NIR -> ir3
without a GLSL round trip.

## Phases

Each phase ends in something observable in the USB log or on the panel. Kernel work is in `xnu/patches/`
(local build, `tools/xnu-local-build.sh 13.5`); every kernel test is one `fastboot boot` with the user present.

0. **Prerequisites.**
   * Pull `a530_pm4.fw`, `a530_pfp.fw`, `a508_zap.mdt/.bNN` from the phone (read only, adb + su); keep out
     of git; deliver through ADT properties as `tools/adt-gpufw.py` does.
   * A shell on the device (tools/ssh over the USB link or a serial-style channel over SWDB): userland
     phases cannot be iterated one boot at a time.
   * Move the RPM client out of sweet_touch.c into its own file with clock/regulator/corner requests.
1. **Power-up, read the chip id.** RPM: `vdd_gfx` (and the dig/mx corners the GPUCC node names); GPUCC:
   gfx3d RCG + branches, rbbmtimer, GCC iface/mem clocks; GDSC cx then gx. Read `RBBM` chip id.
   Reference: Linux `drivers/clk/qcom/gpucc-sdm660.c`, `gdsc.c`, `drivers/gpu/drm/msm/adreno/a5xx_gpu.c`.
   Done when the log shows the GPU's id register.
2. **Zap and the command processor.** PAS/SCM load of `a508_zap` (reuse `sweet_a630_pas.c`, PAS id 13,
   carve-out address from the stock DT), `a5xx_hw_init` (VBIF, UCHE, hwcg, protect), PM4/PFP upload,
   `CP_ME_INIT`, a NOP and a register-write test through the ring.
   Risk: TrustZone refusing the zap on a Motorola-signed chain. The driver never writes
   `RBBM_SECVID_TRUST_CNTL` blindly (an XPU violation resets the SoC).
3. **Memory and submission.** kgsl SMMU v2 page tables (4 KiB), BO allocation, port 0181 + 0182 to
   xnu-8796 as an a5xx service (same drm/msm-shaped user client, `GET_PARAM` answering GPU 508 / chip
   0x05000800 / the a508 GMEM size), a5xx submit tail, polled fences. Test: `gpu/kernel-test` style
   clear + readback from userland.
4. **Mesa on the device.** Mesa freedreno built with a5xx for iOS 16 plain arm64 (no PAC stripping),
   `sweet_drm.c`; offscreen GLES 3.1 triangle and textured quad with readback, then a present to the
   panel through an IOSurface.
5. **CoreAnimation on the GPU (blur, translucency, frame rate).** The Metal device plug-in, first
   generation design (Metal -> Gallium/freedreno, AIR -> NIR): get Chlorophyll's pre-Vulkan revision or
   start from hackios's front end; registered through the IOAcceleratorES stub's `MetalPluginName`
   (0232's mechanism) so backboardd picks it. Fallback if that stalls: the `gpu/eagl` GLES route, which
   needs the QuartzCore hook addresses re-found for iOS 16.7.16 (20H392).
6. **Apps and polish.** Metal for apps system-wide, the GLES framework for GLES apps, zero-copy scanout,
   GPU frequency levels through RPM corners, recovery from hangs.

## Progress

* 2026-11-12: phase 0 done (firmware pulled, SSH over the USB link, `tools/g6plus/install-ssh.sh`); phase 1
  done (`xnu/patches/1044`, `sweetgpu=1`: register write/read-back on the powered GPU, core clock on XO).
  Found on the way: SDM630 has no separate GFX rail (the core is on CX); the gx GDSC needs the gfx3d root
  forced on and both GDSCs must be up before any GPUCC branch turns on.

## Risks, in the order they can stop the work

1. Zap shader / TrustZone (phase 2). 2. SMMU global registers being owned by the hypervisor/TZ (phase 3).
3. Chlorophyll's Gallium-era source not being available, and how much of QuartzCore's Metal use a GLES 3.1
class driver covers (phase 5). 4. Iteration speed: RAM boots only, root over USB, the dead-HELLO flake.
5. Heat and battery: the phone runs from RAM for hours with no power management.

## Size

The original port took about 5 days from first register write to GPU-composited CoreAnimation on the A630,
with an existing driver for a sibling GPU as reference. Here the kernel driver is new (a5xx is simpler: no
GMU), the RPM/clock layer is new, and phase 5 has no working precedent in this repo for iOS 16. Estimate, to
be corrected as phases land: phases 0-2 a few sessions, 3-4 several more, 5 the largest and least certain
(weeks, not days).
