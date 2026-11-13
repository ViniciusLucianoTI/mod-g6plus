/*
 * surfdump: write an IOSurface of another process (looked up by ID) as a PPM, optionally reduced. Debug aid for the
 * render server on the GL shim: its log names the display surfaces ("direct attach of surface N (1125x2436)").
 *   surfdump ID OUT.ppm [DIVISOR]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <IOSurface/IOSurfaceRef.h>

int
main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: surfdump ID OUT.ppm [DIVISOR]\n");
		return 2;
	}
	int div = argc > 3 ? atoi(argv[3]) : 1;
	IOSurfaceRef s = IOSurfaceLookup((IOSurfaceID)strtoul(argv[1], NULL, 0));
	if (!s) {
		fprintf(stderr, "IOSurfaceLookup(%s) failed\n", argv[1]);
		return 1;
	}
	size_t w = IOSurfaceGetWidth(s), h = IOSurfaceGetHeight(s), bpr = IOSurfaceGetBytesPerRow(s);
	IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
	const uint8_t *b = IOSurfaceGetBaseAddress(s);
	FILE *f = fopen(argv[2], "wb");
	if (!b || !f) {
		fprintf(stderr, "no base address or cannot write %s\n", argv[2]);
		return 1;
	}
	fprintf(f, "P6\n%zu %zu\n255\n", w / div, h / div);
	for (size_t y = 0; y < h / div; y++) {
		for (size_t x = 0; x < w / div; x++) {
			const uint8_t *q = b + y * div * bpr + x * div * 4;
			fputc(q[2], f); fputc(q[1], f); fputc(q[0], f);
		}
	}
	fclose(f);
	IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
	printf("surface %s: %zux%zu fourcc %08x\n", argv[1], w, h, (unsigned)IOSurfaceGetPixelFormat(s));
	return 0;
}
