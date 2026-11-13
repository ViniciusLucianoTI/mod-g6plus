/*
 * osmesa_blit: the GPU copy of a rendered texture into an IOSurface (OSMesaSweetBlit, what the EAGL shim's
 * presentRenderbuffer:/attachImage: use), without the shim. Renders a known picture into an FBO texture
 * (left half red, right half green, on a blue clear), copies it into a BGRA IOSurface with and without the
 * vertical flip, and prints a grid of the surface's pixels plus four checks per copy.
 *   on the phone: /usr/local/mesa16/osmesa_blit [SIZE]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <GL/osmesa.h>
#include <GLES2/gl2.h>

extern int OSMesaSweetBlit(GLuint name, int is_texture, uint32_t surface_id, int flip, int opaque);
extern int OSMesaSweetActive(void);
static int g_fail;
#define CHECK(cond, ...) do { if (cond) { printf("PASS  "); } else { printf("FAIL  "); g_fail++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static IOSurfaceRef
make_surface(int w, int h)
{
	int bpr = w * 4, bpe = 4, fmt = 'BGRA';
	CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFNumberRef n;
#define SET(key, v) n = CFNumberCreate(NULL, kCFNumberIntType, &(v)); CFDictionarySetValue(d, key, n); CFRelease(n)
	SET(kIOSurfaceWidth, w); SET(kIOSurfaceHeight, h); SET(kIOSurfaceBytesPerRow, bpr); SET(kIOSurfaceBytesPerElement, bpe); SET(kIOSurfacePixelFormat, fmt);
	IOSurfaceRef s = IOSurfaceCreate(d);
	CFRelease(d);
	return s;
}

int
main(int argc, char **argv)
{
	int w = argc > 1 ? atoi(argv[1]) : 128, h = w;
	setvbuf(stdout, NULL, _IONBF, 0);
	const int attribs[] = { OSMESA_FORMAT, OSMESA_RGBA, OSMESA_DEPTH_BITS, 24, OSMESA_STENCIL_BITS, 8, OSMESA_PROFILE, 0x36,
		                OSMESA_CONTEXT_MAJOR_VERSION, 2, OSMESA_CONTEXT_MINOR_VERSION, 0, 0 };
	OSMesaContext ctx = OSMesaCreateContextAttribs(attribs, NULL);
	unsigned char *buf = calloc((size_t)w * h, 4);
	if (!ctx || !OSMesaMakeCurrent(ctx, buf, GL_UNSIGNED_BYTE, w, h)) {
		printf("FAIL  no context\n");
		return 1;
	}
	printf("INFO  GL_RENDERER %s, hardware path %s\n", (const char *)glGetString(GL_RENDERER), OSMesaSweetActive() ? "active" : "NOT active");

	GLuint tex, fbo;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "FBO complete");
	glViewport(0, 0, w, h);
	glClearColor(0, 0, 1, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_SCISSOR_TEST);
	glScissor(0, 0, w / 2, h / 2);          /* bottom-left quarter (GL coordinates): red */
	glClearColor(1, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glScissor(w / 2, 0, w / 2, h / 2);      /* bottom-right quarter: green */
	glClearColor(0, 1, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_SCISSOR_TEST);
	unsigned char px[4];
	glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	CHECK(px[0] > 250 && px[1] < 5 && px[2] < 5, "glReadPixels of the texture, bottom-left = %d,%d,%d,%d (want red)", px[0], px[1], px[2], px[3]);
	glReadPixels(2, h - 3, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	CHECK(px[2] > 250 && px[0] < 5, "glReadPixels of the texture, top-left = %d,%d,%d,%d (want blue)", px[0], px[1], px[2], px[3]);

	{       /* the source as glReadPixels sees it: how many pixels are one of the three colours */
		unsigned char *all = malloc((size_t)w * h * 4);
		glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, all);
		int good = 0;
		for (int i = 0; i < w * h; i++) {
			const unsigned char *p = all + 4 * i;
			good += (p[0] == 255 && !p[1] && !p[2]) || (!p[0] && p[1] == 255 && !p[2]) || (!p[0] && !p[1] && p[2] == 255);
		}
		CHECK(good == w * h, "source texture by glReadPixels: %d of %d pixels are pure red/green/blue", good, w * h);
		free(all);
	}
	for (int flip = 0; flip <= 1; flip++) {
		IOSurfaceRef s = make_surface(w, h);
		if (!s) {
			printf("FAIL  IOSurfaceCreate\n");
			return 1;
		}
		IOSurfaceLock(s, 0, NULL);
		memset(IOSurfaceGetBaseAddress(s), 0x55, (size_t)IOSurfaceGetBytesPerRow(s) * h);    /* a value no test colour has */
		IOSurfaceUnlock(s, 0, NULL);
		int r = OSMesaSweetBlit(tex, 1, IOSurfaceGetID(s), flip, getenv("BLIT_ALPHA") ? 0 : 1);
		printf("INFO  OSMesaSweetBlit(flip %d) -> %d, surface id %u bpr %zu\n", flip, r, IOSurfaceGetID(s), IOSurfaceGetBytesPerRow(s));
		IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
		const unsigned char *b = IOSurfaceGetBaseAddress(s);
		size_t bpr = IOSurfaceGetBytesPerRow(s);
		/* surface rows are top-down; BGRA in memory. Grid of 4x4 samples, printed as R,G,B */
		for (int gy = 0; gy < 4; gy++) {
			printf("      row %3d:", gy * h / 4 + h / 8);
			for (int gx = 0; gx < 4; gx++) {
				const unsigned char *p = b + (size_t)(gy * h / 4 + h / 8) * bpr + (size_t)(gx * w / 4 + w / 8) * 4;
				printf("  %3d,%3d,%3d", p[2], p[1], p[0]);
			}
			printf("\n");
		}
		/* GL bottom-left is red. Without flip surface row 0 = GL row 0 (red at the top-left); with flip red at the bottom-left. */
		const unsigned char *red = b + (size_t)(flip ? h - 3 : 2) * bpr + 2 * 4, *blue = b + (size_t)(flip ? 2 : h - 3) * bpr + 2 * 4;
		const unsigned char *green = b + (size_t)(flip ? h - 3 : 2) * bpr + (size_t)(w - 3) * 4;
		{
			int good = 0, untouched = 0;
			for (int y = 0; y < h; y++) {
				for (int x = 0; x < w; x++) {
					const unsigned char *p = b + (size_t)y * bpr + (size_t)x * 4;
					good += (p[2] == 255 && !p[1] && !p[0]) || (!p[2] && p[1] == 255 && !p[0]) || (!p[2] && !p[1] && p[0] == 255);
					untouched += p[0] == 0x55 && p[1] == 0x55 && p[2] == 0x55;
				}
			}
			printf("INFO  flip %d: %d of %d surface pixels are pure red/green/blue, %d still hold the 0x55 fill\n", flip, good, w * h, untouched);
		}
		for (int again = 0; again < 3; again++) {       /* does the CPU's view change with time (stale cache lines) or not (what the GPU wrote)? */
			static volatile char churn[8 << 20];
			memset((void *)churn, again, sizeof(churn));
			usleep(300000);
			int good = 0;
			for (int y = 0; y < h; y++) {
				for (int x = 0; x < w; x++) {
					const unsigned char *p = b + (size_t)y * bpr + (size_t)x * 4;
					good += (p[2] == 255 && !p[1] && !p[0]) || (!p[2] && p[1] == 255 && !p[0]) || (!p[2] && !p[1] && p[0] == 255);
				}
			}
			printf("INFO  flip %d: %d pure pixels %d ms later\n", flip, good, 300 * (again + 1));
		}
		if (r) {
			CHECK(red[2] > 250 && red[1] < 5 && red[0] < 5, "flip %d: red where expected (R,G,B %d,%d,%d)", flip, red[2], red[1], red[0]);
			CHECK(green[1] > 250 && green[2] < 5, "flip %d: green where expected (%d,%d,%d)", flip, green[2], green[1], green[0]);
			CHECK(blue[0] > 250 && blue[2] < 5, "flip %d: blue where expected (%d,%d,%d)", flip, blue[2], blue[1], blue[0]);
		} else {
			printf("INFO  flip %d: the GPU copy was refused (the caller would copy on the CPU)\n", flip);
		}
		{       /* the whole surface as a picture, rows as stored (top-down) */
			char name[64];
			snprintf(name, sizeof(name), "/tmp/blit-flip%d.ppm", flip);
			FILE *fp = fopen(name, "wb");
			if (fp) {
				fprintf(fp, "P6\n%d %d\n255\n", w, h);
				for (int y = 0; y < h; y++) {
					for (int x = 0; x < w; x++) {
						const unsigned char *p = b + (size_t)y * bpr + (size_t)x * 4;
						fputc(p[2], fp); fputc(p[1], fp); fputc(p[0], fp);
					}
				}
				fclose(fp);
			}
		}
		IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
		CFRelease(s);
	}
	printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
	return g_fail;
}
