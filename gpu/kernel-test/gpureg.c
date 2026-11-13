/*
 * gpureg: read or write one Adreno register through the SweetAdreno630 debug selector (kernel boot-arg
 * sweetgpupoke=1; xnu/patches 1055). A bring-up aid for trying register hypotheses without a kernel build.
 *   gpureg REG            read  (REG is the register index as in a5xx.xml, hex)
 *   gpureg REG VALUE      write, then read back
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
		fprintf(stderr, "usage: gpureg REG [VALUE]\n");
		return 2;
	}
	io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceNameMatching(SWEET_GPU_SERVICE_NAME));
	io_connect_t c = 0;
	if (!s || IOServiceOpen(s, mach_task_self(), 0, &c) != 0) {
		fprintf(stderr, "no %s\n", SWEET_GPU_SERVICE_NAME);
		return 1;
	}
	struct sweet_gpu_debug d;
	memset(&d, 0, sizeof(d));
	d.w[36] = (uint32_t)strtoul(argv[1], NULL, 16);
	d.w[39] = argc > 2 ? 0x504f4b45u : 0x5045454bu;
	if (argc > 2) {
		d.w[37] = (uint32_t)strtoul(argv[2], NULL, 16);
	}
	size_t o = sizeof(d);
	kern_return_t kr = IOConnectCallStructMethod(c, SWEET_GPU_SEL_DEBUG, &d, sizeof(d), &d, &o);
	if (kr != 0 || d.w[39] != 0x444f4e45u) {
		fprintf(stderr, "not done (kr %#x): is the kernel booted with sweetgpupoke=1?\n", kr);
		return 1;
	}
	printf("reg 0x%04x = 0x%08x\n", d.w[36], d.w[37]);
	return 0;
}
