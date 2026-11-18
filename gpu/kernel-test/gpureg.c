/*
 * gpureg: read or write one Adreno register through the SweetAdreno630 debug selector (kernel boot-arg
 * sweetgpupoke=1; xnu/patches 1055). A bring-up aid for trying register hypotheses without a kernel build.
 *   gpureg REG            read  (REG is the register index as in a5xx.xml, hex)
 *   gpureg REG VALUE      write, then read back
 *   gpureg freq           core clock now (MHz), hottest SoC sensor, level changes (xnu/patches 1058)
 *   gpureg cpu big|little MHZ|boot   CPU cluster clock (xnu/patches 1062; limits in the kernel: big <= 2016, little <= 1536)
 *   gpureg freq MHZ|auto  pin the core clock at a level (160 240 370 465 588 647 700) or hand it to the governor
 */
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../xnu/gpu-abi/sweet_adreno_abi.h"

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: gpureg REG [VALUE] | gpureg freq [MHZ|auto] | gpureg cpu big|little MHZ|boot\n");
		return 2;
	}
	int freq = !strcmp(argv[1], "freq");
	int cpu = !strcmp(argv[1], "cpu");
	io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceNameMatching(SWEET_GPU_SERVICE_NAME));
	io_connect_t c = 0;
	if (!s || IOServiceOpen(s, mach_task_self(), 0, &c) != 0) {
		fprintf(stderr, "no %s\n", SWEET_GPU_SERVICE_NAME);
		return 1;
	}
	struct sweet_gpu_debug d;
	memset(&d, 0, sizeof(d));
	if (cpu) {
		if (argc < 4 || (strcmp(argv[2], "big") && strcmp(argv[2], "little"))) {
			fprintf(stderr, "usage: gpureg cpu big|little MHZ|boot\n");
			return 2;
		}
		d.w[39] = 0x43505546u;         /* 'CPUF' */
		d.w[36] = !strcmp(argv[2], "big");
		d.w[37] = !strcmp(argv[3], "boot") ? 1u : (uint32_t)strtoul(argv[3], NULL, 10);
		size_t oc = sizeof(d);
		kern_return_t kc = IOConnectCallStructMethod(c, SWEET_GPU_SEL_DEBUG, &d, sizeof(d), &d, &oc);
		printf("asked for the %s cluster at %s%s (kr %#x); the result is in the kernel log (SWEET CPUCLK)\n", argv[2], argv[3], strcmp(argv[3], "boot") ? " MHz" : "", kc);
		return kc != 0;
	}
	if (freq) {
		d.w[39] = 0x46524551u;         /* 'FREQ' */
		d.w[36] = argc > 2 ? (!strcmp(argv[2], "auto") ? 1u : (uint32_t)strtoul(argv[2], NULL, 10)) : 0u;
	} else {
		d.w[36] = (uint32_t)strtoul(argv[1], NULL, 16);
		d.w[39] = argc > 2 ? 0x504f4b45u : 0x5045454bu;
		if (argc > 2) {
			d.w[37] = (uint32_t)strtoul(argv[2], NULL, 16);
		}
	}
	size_t o = sizeof(d);
	kern_return_t kr = IOConnectCallStructMethod(c, SWEET_GPU_SEL_DEBUG, &d, sizeof(d), &d, &o);
	if (kr != 0 || d.w[39] != 0x444f4e45u) {
		fprintf(stderr, "not done (kr %#x): is the kernel booted with sweetgpupoke=1?\n", kr);
		return 1;
	}
	if (freq) {
		printf("core clock %u MHz%s, hottest sensor %d.%d C, %u level changes\n", d.w[36], d.w[36] ? "" : " (the bring-up clock, not on a GPU PLL)",
		    (int)d.w[37] / 10, (int)d.w[37] >= 0 ? (int)d.w[37] % 10 : 0, d.w[38]);
		return 0;
	}
	printf("reg 0x%04x = 0x%08x\n", d.w[36], d.w[37]);
	return 0;
}
