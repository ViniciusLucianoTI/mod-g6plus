#pragma once
/*
 * Board selection: build.sh passes -DBOARD_BERYLLIUM for the Poco F1 (xiaomi beryllium, SDM845),
 * -DBOARD_EVERT for the Moto G6 Plus (motorola evert, SDM630); the default is sweet (SM7150).
 * Every value comes from the board's mainline/downstream device tree.
 *
 * Per-board knobs beyond addresses:
 *   MMU_GRANULE_4K	the CPU has no 16K translation granule (Cortex-A53 class: Kryo 260 Silver)
 *   STORAGE_EMMC	boot storage is eMMC behind an SDHCI host (else UFS)
 *   SPMI_ARB_V3	PMIC arbiter v2/v3 register layout (else v5)
 *   PMIC_SID_*	SPMI slave IDs of the PON, charger/FG and WLED peripherals
 */
#include <stdint.h>

#if defined(BOARD_BERYLLIUM)
#define BOARD_NAME		"beryllium"
/* reserved-memory cont_splash_mem@9d400000, simple-framebuffer 1080x2246 */
#define FB_BASE			0x9d400000UL
#define FB_W			1080
#define FB_H			2246
/* sdm845 imem@146bf000, restart_reason@65c */
#define IMEM_RESTART_REASON	0x146bf65cUL
#elif defined(BOARD_EVERT)
#define BOARD_NAME		"evert"
/*
 * LineageOS msm8998 kernel, sdm630.dtsi: cont_splash_region@9d400000 (0x23ff000 bytes); panel
 * mipi_mot_vid_tianma_1080p_593, 1080x2160 video mode (measured, docs/G6PLUS_HARDWARE.md 0).
 * ASSUMPTION, not measured: ABL leaves the splash as a8r8g8b8 with stride FB_W*4 like on SDM845.
 */
#define FB_BASE			0x9d400000UL
#define FB_W			1080
#define FB_H			2160
/* sdm630.dtsi qcom,msm-imem@146bf000, restart_reason@65c */
#define IMEM_RESTART_REASON	0x146bf65cUL
#define MMU_GRANULE_4K		1
#define STORAGE_EMMC		1
#define SPMI_ARB_V3		1
#else
#define BOARD_NAME		"sweet"
/* reserved-memory cont_splash_region@9c000000 */
#define FB_BASE			0x9c000000UL
#define FB_W			1080
#define FB_H			2400
/* qcom,msm-imem restart_reason@65c */
#define IMEM_RESTART_REASON	0x146aa65cUL
#endif

#define FB_STRIDE		(FB_W * 4)	/* a8r8g8b8, matches mainline simplefb */
#define FB_PHYS			FB_BASE

#if defined(BOARD_EVERT)
/* sdm630.dtsi: restart@10ac000 (qcom,pshold), intc GICD 0x17a00000 + GICR 0x17b00000 (8 x 128K) */
#define PSHOLD			0x010ac000UL
#define GICD_BASE		0x17a00000UL
#define GICR_BASE		0x17b00000UL
#define GICR_SIZE		0x100000UL
/* sdm660-common.dtsi sdhc_1: sdhci@c0c4000 (qcom,sdhci-msm-v5: hc_mem = standard SDHCI registers), 8 bit */
#define EMMC_BASE		0x0c0c4000UL
/* sdm630.dtsi qcom,spmi@800f000: core, chnls, obsrvr, cnfg (intr 0x0a400000 unused), qcom,ee = 0 */
#define SPMI_CORE		0x0800f000UL
#define SPMI_CHNLS		0x08400000UL
#define SPMI_OBSRVR		0x09400000UL
#define SPMI_CNFG		0x0800a000UL
/* msm-pm660.dtsi / msm-pm660l.dtsi: PM660 = SID 0 (PON 0x800, SMB2 0x1000.., FG gen3 0x4000..), PM660L WLED = SID 3 */
#define PMIC_SID_PON		0
#define PMIC_SID_CHG		0
#define PMIC_SID_WLED		3
/* MPIDR of CPU n for PSCI CPU_ON: two clusters of four (Aff1 = cluster, Aff0 = core) */
#define CPU_MPIDR(n)		((((uint64_t)(n) >> 2) << 8) | ((uint64_t)(n) & 3))
#else
/* Same on SM7150 and SDM845 */
#define PSHOLD			0x0c264000UL
#define GICD_BASE		0x17a00000UL
#define GICR_BASE		0x17a60000UL
#define GICR_SIZE		0x100000UL
#define UFS_BASE		0x01d84000UL	/* ufshc@1d84000 */
/* SPMI PMIC arbiter v5 */
#define SPMI_CORE		0x0c440000UL	/* +0 version, +4 APID count, +0x900 + 4*apid ppid map */
#define SPMI_CHNLS		0x0c600000UL	/* read/write channels, 0x10000 per APID */
#define SPMI_OBSRVR		0x0e600000UL	/* observer, 0x80 per APID */
#define SPMI_CNFG		0x0c40a000UL	/* +0x700 + 4*apid: owner EE */
/* PM8998 = SID 0 (PON), PMI8998 = SID 2 (SMB2 + FG gen3) and SID 3 (WLED) */
#define PMIC_SID_PON		0
#define PMIC_SID_CHG		2
#define PMIC_SID_WLED		3
/* DynamIQ-style numbering: Aff1 = core */
#define CPU_MPIDR(n)		((uint64_t)(n) << 8)
#endif

/* Translation granule of mmu.c; also the rounding of every range it maps */
#if defined(MMU_GRANULE_4K)
#define MMU_PAGE		0x1000UL
#else
#define MMU_PAGE		0x4000UL
#endif
