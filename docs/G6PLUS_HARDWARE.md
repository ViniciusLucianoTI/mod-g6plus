# Moto G6 Plus (evert) hardware facts, and what Bergamota hardcodes for the Poco F1

Status 2026-11-10: desk research plus **one collection from the user's phone (section 0)**. Marks:
**(src)** read in public source, **(inf)** inference, **(dev)** must be read on the device
(`tools/g6plus/collect.sh`, Enxerto's diagnostic screens).

Sources: LineageOS `android_device_motorola_evert` + `android_device_motorola_msm8998-common`
(lineage-22.2; evert inherits msm8998-common), `android_kernel_motorola_msm8998` (4.4, DTS under
`arch/arm/boot/dts/qcom/sdm630*`), mainline `arch/arm64/boot/dts/qcom/sdm630.dtsi`,
`lineage_wiki/_data/devices/evert.yml`. Spot-checked by hand: msm-id/board-id, `c0c4000.sdhci`,
`Image.gz-dtb`, A/B, pagesize, 8x `arm,cortex-a53`.

## 0. Measured on the user's phone (2026-11-10, `tools/g6plus/collect.sh`, Magisk root)

Device: XT1926-8, hwrev 0xC100 / pvt1, radio Brazil, carrier retbr, LineageOS 22 (Android 15),
kernel 4.4.302, bootloader `MBM-3.0-evert_retail-83f0397e7a-200825`, slot `_a`, unlocked
(`bl_state=2`, verifiedbootstate orange). Raw data: `backup/evert/20261110-235509/` (git-ignored).

* **CPU: 8x MIDR implementer 0x51 (Qualcomm), part 0x801, variant 0xa, revision 4** = Kryo 260 Silver-class,
  an A53 derivative (not plain ARM 0xd03). `Features: fp asimd evtstrm aes pmull sha1 sha2 crc32`:
  **no `atomics` (LSE), no `lrcpc`, no `fphp/asimdhp`, no `asimddp`** -> ARMv8.0 + crypto, confirmed.
  The LSE blocker in section 2 is real. ID_AA64MMFR0 (16K granule) not readable from EL0 on this 4.4
  kernel; read it from Enxerto.
* Topology: cpu0-3 = package 1, max 2.208 GHz (boot cluster); cpu4-7 = package 0, max 1.8432 GHz.
* RAM: 4 GB model (MemTotal 3.83 GB). System RAM ranges 0x80000000-0x855fffff, 0x88f00000-0x8abfffff,
  0x94b00000-0xaefcefff, 0xaf0c0000-0x17e4bffff.
* Storage: eMMC `mmcblk0` 59640 MiB (64 GB) behind `c0c4000.sdhci`; SD at `c084000.sdhci`. The UFS PHY
  (0x1da7000) and UFS ICE exist in the DT but no UFS host is bound. Rootfs `root=/dev/mmcblk0p67` (system_a).
* **Partitions (70, A/B):** `xbl_a/b` (p1/p2), `tz`, `rpm`, `hyp`, `devcfg`, `pmic`, **`abl_a/b` (p13/14, 1 MiB)**
  -> the bootloader is Qualcomm XBL + ABL (Motorola "MBM-3.0"), like SDM845. `boot_a/b` p44/p45 64 MiB,
  **`dto_a/b` p46/47 16 MiB** (DT overlay partition, check whether it is used), `logo_a/b` p56/57 32.6 MiB,
  `vbmeta_a/b`, `modem_a/b` 110 MiB, `modemst1/2`, `fsg_a/b`, `persist` 32 MiB, `utags`, `hw`, `oem_a/b` 240 MiB,
  `system_a/b` 2752 MiB, `vendor_a/b` 912 MiB, **`userdata` p69 51200 MiB**.
* Display: panel `mipi_mot_vid_tianma_1080p_593` (`mdss_mdp.panel=1:dsi:0:qcom,mdss_dsi_mot_tianma_593_1080p_vid_v0`),
  1080x2160 @60, video mode, fb0 virtual 1080x4320 (double buffer). MDSS is the old fbdev `mdss_fb` driver.
* **Touch: Goodix GT9xx at i2c-3 0x5d (`gt9xx-mmi`, bound to `Goodix-TS`).** The FocalTech node at 3-0038
  is present but unbound (not fitted).
* Other I2C: i2c-2 Cypress SAR sensor `sf3155` @0x1d, Cirrus `cs35l35` amp @0x40; i2c-6 NXP NFC @0x28.
* Keys: `qpnp_pon` input (power, vol-down) + gpio-keys.
* GPU: `Adreno508v1`, 700 MHz max; firmware present: `a530_pfp.fw`, `a530_pm4.fw`, `a508_zap.mdt/.b00-.b02`.
* MMIO seen in /proc/iomem: GCC 0x00100000, PS_HOLD 0x010ac000, TLMM 0x03000000-0x03bfffff, MSS qdsp6
  0x04080000, KGSL 0x05000000, GPU SMMU 0x05040000, GPUCC 0x05065000, **SPMI cnfg 0x0800a000, core 0x0800f000,
  chnls 0x08400000, obsrvr 0x09400000, intr 0x0a400000**, DWC3 0x0a800000, QMP USB3 PHY 0x0c010000,
  QUSB2 0x0c012000 (+ second 0x0c014000).
* Physical note: the phone's USB data link is flaky (first it enumerated only as a charger; later it dropped
  off USB mid-session). Clean the port / try another cable before any flashing.

## 1. G6 Plus vs Poco F1

| Area | Poco F1 (beryllium, SDM845) | Moto G6 Plus (evert, SDM630) |
|---|---|---|
| CPU | 4x A75 + 4x A55, ARMv8.2, MPIDR `0x81000n00` | **8x Cortex-A53, ARMv8.0** (src). Clusters: perf cpu@100-103 (boot cluster, 2.208 GHz), pwr cpu@0-3 (1.843 GHz) (src). MIDR, ID_AA64* (dev) |
| GIC / timer | GICv3, GICD 0x17a00000, GICR 0x17a60000; 19.2 MHz | GICv3, GICD 0x17a00000, **GICR 0x17b00000** (stride 0x20000); 19.2 MHz; PSCI 1.0 smc (src) |
| DRAM | 0x80000000, 8 GB | 0x80000000, 4/6 GB (src). Reserved: wlan_msa 0x85700000, modem 0x8ac00000 (+0x7e00000), adsp 0x92a00000, mba 0x94800000, **cont_splash 0x9d400000 (+0x23ff000)**, ramoops 0xaf000000 (src) |
| Storage | UFS 0x01d84000 | **eMMC, SDHCI v5 at 0x0c0c4000** (cmdq 0x0c0c5000), 8-bit (src). SD card sdhc_2 0x0c084000 |
| Partitions | single slot, header v1 boot | **A/B** (boot_a/b, system_a/b, vendor_a/b), no dynamic partitions, no recovery partition (recovery-as-boot), boot 64 MiB (src) |
| Boot image | v1, Image.gz-dtb | **v0**, Image.gz-dtb (appended DTBs), base 0, pagesize 4096 (src) |
| DTB select | msm-id <0x141 0x20001>, board-id <0x45 0> | **msm-id <318 0>, board-id retail PVT <0x46 0xC100>/<0x47 0xC100>** (src) |
| Bootloader | Qualcomm ABL, `fastboot boot` | `fastboot boot` works (src, TWRP docs). ABL/UEFI vs LK: likely ABL (inf), (dev) |
| Display | DPU 4.0 at 0x0ae00000, 1080x2246 | **MDP5 / DPU 3.3**: MDSS 0x0c900000, MDP 0x0c901000, DSI0 0x0c994000 (14 nm PHY); **1080x2160** video mode, 4 lanes; panels Tianma (default) / AUO / DJN (src). Splash stride/format (dev) |
| Backlight | PMI8998 WLED | **PM660L WLED** (src) |
| Touch | NT36672A @0x01 (or FT8719 @0x38), GENI I2C14, bit-banged GPIO 33/34 | **FocalTech @0x38 or Goodix GT9xx @0x5d on BLSP1 QUP3 (i2c_3)**, IRQ GPIO 67, reset GPIO 66, supply pm660l_l3 (src). Which one is fitted (dev) |
| PMIC | PM8998 + PMI8998, SPMI arb v5 at 0x0c440000 | **PM660 (sid 0/1) + PM660L (sid 2/3)**, SPMI at **0x0800f000**; arbiter version likely v3 (inf), (dev) |
| Keys | PON KPDPWR / RESIN, vol-up PM8998 GPIO6 | PON KPDPWR (power), RESIN (vol-down), **vol-up = PM660L GPIO7** (src) |
| Charger / FG | PMI8998 SMB2 + FG gen3, 4000 mAh | **PM660 SMB2 + FG gen3** (same IP family, other SID), **3200 mAh, 4.4 V** (src) |
| Power mgmt | RPMh | **RPM over SMD** (no RPMh) (src, SoC family) |
| Serial buses | GENI QUPv3 | **BLSP QUP v2 + UARTDM v1.4**; debug UART BLSP1 UART2 0x0c170000, GPIO 4/5 (src); no known pads |
| USB | DWC3 0x0a600000, QUSB2 0x088e2000, SMMU 0x15000000 sid 0x740 | **DWC3 0x0a800000**, QUSB2 0x0c012000, QMP 0x0c010000 (src); GCC sdm660 clocks; SMMU/SID (dev) |
| GPU | Adreno 630 + GMU, 0x05000000 | **Adreno 508 (a5xx), no GMU**, 0x05000000, SMMU 0x05040000, fw a530_pm4/pfp + a508_zap (src) |
| Wi-Fi / BT | WCN3990 | **WCN3990** (src): the same chip, but the modem/MSS bring-up around it differs |
| Audio / other | WCD934x | Cirrus CS47L35 on SPI, NXP NFC, FPC fingerprint (src) |

## 2. The real blocker: the CPU, not the drivers

On Poco F1 the iOS userland runs natively except PAC (rewritten by Renegade) and FJCVTZS (emulated).
The A53 is ARMv8.0 and lacks instructions Apple compiles in unconditionally:

* **LSE atomics** (v8.1): ~426k sites in the iOS 18 dyld shared cache, ~782k in iOS 26
  (`docs/IOS18_FEASIBILITY.md`, `docs/IOS26_FEASIBILITY.md`).
* **LDAPR / RCpc** (v8.3): ~93.7k sites. FP16, DotProd, RDM, DC CVAP also used.
* Renegade rewrites 4 bytes for 4 bytes. LSE ops have no single-word v8.0 equivalent (they need an
  LL/SC loop), so they cannot be rewritten in place. LDAPR -> LDAR is a 1:1 rewrite (stronger ordering,
  same result) and is cheap; LSE is the hard part.
* Options: (a) trap-and-emulate every LSE op in the kernel (as 1019 does for PAC; correct but each
  atomic costs a trap, and atomics are hot: likely unusable performance); (b) out-of-line rewrite of
  the shared cache (branch to an LL/SC trampoline: changes code layout, a major new Renegade stage);
  (c) a different, older iOS userland built for ARMv8.0 (iOS <= 10-era 64-bit devices; would
  abandon the Bergamota stack). **Decide this before writing drivers.**
* **Chosen 2026-11-10 (user): option (a) first, to measure.** Implemented as
  `xnu/patches-12377/1020-sweet-lse-emulation.patch` + `tools/lseemu/` (host-verified against the Mac's
  LSE hardware; not yet run on the phone, which needs a G6-bootable kernel first). Its log gives the
  trap rate per class and process, which decides whether (b) is needed.

Kernel side (fixable): `0004-sweet-machine-config.patch` builds XNU with `-march=armv8.2-a+crypto`
(LSE can be emitted) -> `armv8-a+crypto`; `__ARM_PAN_AVAILABLE__ 1` must go (no PAN on A53);
SMP code panics unless Aff0 == 0 (`0065`, `0078`; A53 MPIDRs are 0x000-0x003 / 0x100-0x103);
16K granule: sources conflict; the A53 TRM's ID_AA64MMFR0 reset value 0x1122 means TGran16 = 0, i.e. **no 16K granule** (inf), so XNU would need 4K hardware pages under 16K VM pages, as the SDM429 fork did (`docs/FORKS_SURVEY.md`, `0101-t290-a53-4k-gicv2`); Enxerto's MMU (`enxerto/mmu.c`) is 16K too. Confirm ID_AA64MMFR0 (dev).

## 3. Where Bergamota hardcodes the Poco F1

Almost everything is a compile-time constant; Enxerto reads only `/chosen` (bootargs, initrd) from the
Qualcomm DTB, and XNU only sees the ADT from `tools/mkadt.py`. The board switch is
`sweet_board_beryllium()` (patch 0082, `bsd/dev/sweet_board.h`, ADT compatible `xiaomi,beryllium`);
evert needs a third board everywhere.

| Area | Files (12377 numbering) |
|---|---|
| Memory map | `enxerto/board.h` (FB 0x9d400000, IMEM), `enxerto/bergamota.c` (DL_BASE 0xb8000000), `enxerto/loader.c`, `tools/mkpayload.py`, `tools/mkadt.py` (PHYS_BASE, lowpool), xnu 0127, 0082, 0010, carve-outs in 0103/0104/1106/1111/1113/1117/1118 |
| CPU / SMP | `enxerto/enxerto.c` (PSCI, `i<<8`), `tools/mkadt.py` (cpu nodes, kryo385), xnu 0065/0078 (Aff0 panic), `start.s`, SWEET.h, cpufreq 0093/0190/0192 (cpufreq-hw; SDM630 uses OSM/APCS), 0191 |
| GIC / timer | `enxerto/board.h`, `enxerto/enxerto.c`, `tools/mkadt.py`, xnu 0004/0017 |
| Storage | `enxerto/ufs.c`, `storage.c` (GPT per UFS LUN), `tools/mkadt.py` (sweet-ufs), xnu `sweet_ufs.c` (0028/0030/0034/0069/0173/0222), `rd=ufsroot0` in `tools/renegade/mkboot-full.sh` -> **full replacement with SDHCI/eMMC** |
| USB | `enxerto/usb_dwc3.c`, `usb_power.c` (gcc-sdm845 offsets, QUSB2 init seq, SMMU sid), xnu 0072/0074/0087/0089/0094/0098/0109 |
| Display | `enxerto/board.h`, `charge.c`, `fbui.c` (1080x2246, notch), xnu 0096/0126 (DPU offsets, pipes), WLED in `enxerto/pmic.c` + 0079/0126 |
| Touch | xnu 0082b/0085/0101/0111-0114/0161/1010 (NT36672A/FT8719, GENI SE, bit-bang GPIOs) |
| Keys / reboot / PMIC | `enxerto/pmic.c` (**rejects SPMI arbiter < v5**), `charger.c`, `charge_mode.c`, xnu 0060/0062/0082b/0092/0095/0130/0137/0140/0200-0226 |
| Clocks / regulators | GCC 0x00100000 with sdm845 offsets everywhere; `sweet_rpmh.c` (0062/0104) -> RPM-SMD on SDM630 |
| GPU | xnu 0103/0180-0189/0214-0217/0240, `tools/adt-gpufw.py`, `gpu/` (A630 + Mesa) -> a5xx rewrite |
| Wi-Fi / BT | xnu 0104/0124/1104-1130, `tools/adt-wlan.py`, `tools/adt-bluetooth.py` |
| Boot image / board | `enxerto/build.sh` (BOARD=beryllium, header v1), `tools/extract-dtb.py`, `tools/renegade/mkboot-full.sh`, `apps/Bergamota` (`BuildModel.swift`, `AppSettings.swift`, `BootQueue.swift`) |

## 4. Still to read on the device

1. `/proc/cpuinfo` + ID registers: MIDR per core, ID_AA64ISAR0/1 (LSE, RCpc, FP16, DotProd absent?),
   ID_AA64MMFR0 (TGran16, PARange), crypto present.
2. Bootloader: unlocked? ABL or LK (`fastboot getvar all`)? Does ABL accept a v0 image with our own
   payload? Where does it load the Image and ramdisk?
3. `/sys/firmware/fdt` -> `dtb/evert/live.dtb`, and the stock `boot_a`/`boot_b` -> `extract-dtb.py`.
4. Memory node and full reserved-memory; splash stride and format; which panel is fitted.
5. Touch IC fitted (FocalTech or Goodix), SPMI arbiter version, USB SMMU stream ID.
6. Partition table (`ls -l /dev/block/by-name`), and partition backups (`collect.sh --backup`)
   before anything is flashed.
