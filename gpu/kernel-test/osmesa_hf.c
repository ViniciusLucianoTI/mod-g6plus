/*
 * osmesa_hf: osmesa_tri with half-float vertex colours (ES 3 context). An offscreen OpenGL ES 2 context through OSMesa
 * (gpu/eagl/mesa/build-mesa.sh), a clear and one shaded triangle, glReadPixels, and a check of five pixels.
 * With the hardware screen (SweetAdreno630 present and SWEET_GPU not 0) GL_RENDERER names the Adreno and the
 * pixels come from the GPU; SWEET_GPU=0 runs the same thing on softpipe for comparison.
 *
 *   TARGET=ios16-arm64 gpu/kernel-test/build.sh        -> out/osmesa_tri (needs GPU_EAGL_WORK, see build.sh)
 *   on the phone: /usr/local/bin/osmesa_tri [WIDTH HEIGHT] [-ppm FILE]
 * Exit status = number of failed checks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <GL/osmesa.h>
#include <GLES2/gl2.h>

static int g_fail;
#define CHECK(cond, ...) do { if (cond) { printf("PASS  "); } else { printf("FAIL  "); g_fail++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static GLuint
shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok = 0;
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512] = "";
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		printf("FAIL  shader compile: %s\n", log);
		g_fail++;
	}
	return s;
}

static int
near(const unsigned char *p, int r, int g, int b)
{
	return abs(p[0] - r) <= 8 && abs(p[1] - g) <= 8 && abs(p[2] - b) <= 8;
}

static double
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int
main(int argc, char **argv)
{
	int w = 256, h = 256;
	const char *ppm = NULL;
	setvbuf(stdout, NULL, _IONBF, 0);       /* a crash must not swallow what was printed */
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-ppm") && i + 1 < argc) {
			ppm = argv[++i];
		} else if (i + 1 < argc && atoi(argv[i]) > 0 && atoi(argv[i + 1]) > 0) {
			w = atoi(argv[i]);
			h = atoi(argv[i + 1]);
			i++;
		}
	}
	const int attribs[] = {
		OSMESA_FORMAT, OSMESA_RGBA, OSMESA_DEPTH_BITS, 24, OSMESA_STENCIL_BITS, 8,
		OSMESA_PROFILE, 0x36 /* Bergamota Mesa patch: OpenGL ES profile */, OSMESA_CONTEXT_MAJOR_VERSION, 3, OSMESA_CONTEXT_MINOR_VERSION, 0, 0
	};
	OSMesaContext ctx = OSMesaCreateContextAttribs(attribs, NULL);
	CHECK(ctx != NULL, "OSMesaCreateContextAttribs");
	if (!ctx) {
		return 1;
	}
	unsigned char *buf = calloc((size_t)w * h, 4), *px = calloc((size_t)w * h, 4);
	CHECK(OSMesaMakeCurrent(ctx, buf, GL_UNSIGNED_BYTE, w, h) == GL_TRUE, "OSMesaMakeCurrent %dx%d", w, h);
	const char *renderer = (const char *)glGetString(GL_RENDERER), *version = (const char *)glGetString(GL_VERSION);
	printf("INFO  GL_VENDOR   %s\nINFO  GL_RENDERER %s\nINFO  GL_VERSION  %s\n", (const char *)glGetString(GL_VENDOR),
	    renderer ? renderer : "(null)", version ? version : "(null)");
	int hw = renderer && (strstr(renderer, "Adreno") || strstr(renderer, "FD5") || strstr(renderer, "FD6"));
	printf("INFO  %s\n", hw ? "hardware renderer (freedreno)" : "software renderer");

	static const char *vs = "attribute vec2 p; attribute vec3 c; varying vec3 v; void main() { v = c; gl_Position = vec4(p, 0.0, 1.0); }";
	static const char *fs = "precision mediump float; varying vec3 v; void main() { gl_FragColor = vec4(v, 1.0); }";
	GLuint prog = glCreateProgram();
	glAttachShader(prog, shader(GL_VERTEX_SHADER, vs));
	glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(prog, 0, "p");
	glBindAttribLocation(prog, 1, "c");
	glLinkProgram(prog);
	GLint linked = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &linked);
	CHECK(linked, "program linked");
	glUseProgram(prog);

	/* a triangle covering the lower left half: (-1,-1) red, (1,-1) green, (-1,1) blue */
	static const GLfloat pos[] = { -1, -1, 1, -1, -1, 1 };
	static const GLushort col[] = { 0x3c00, 0, 0, 0, 0x3c00, 0, 0, 0, 0x3c00 };   /* half floats, as CoreAnimation passes vertex colours */
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
	glVertexAttribPointer(1, 3, 0x140b /* GL_HALF_FLOAT */, GL_TRUE, 0, col);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glViewport(0, 0, w, h);

	double t0 = now_ms();
	int frames = 20;
	for (int f = 0; f < frames; f++) {
		glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glFinish();
	}
	double t1 = now_ms();
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
	CHECK(glGetError() == GL_NO_ERROR, "no GL error after %d frames (%.2f ms per clear + triangle + finish)", frames, (t1 - t0) / frames);

#define PX(x, y) (px + 4 * ((size_t)(y) * w + (x)))
	const unsigned char *bg = PX(w - 2, h - 2), *r = PX(2, 2), *g = PX(w - 6, 2), *b = PX(2, h - 6), *mid = PX(w / 4, h / 4);
	CHECK(near(bg, 64, 128, 191), "background (top right) = %d,%d,%d (want 64,128,191: the clear colour)", bg[0], bg[1], bg[2]);
	CHECK(r[0] > 230 && r[1] < 20 && r[2] < 20, "corner (-1,-1) = %d,%d,%d (want red)", r[0], r[1], r[2]);
	CHECK(g[1] > 220 && g[0] < 30 && g[2] < 30, "corner (1,-1) = %d,%d,%d (want green)", g[0], g[1], g[2]);
	CHECK(b[2] > 220 && b[0] < 30 && b[1] < 30, "corner (-1,1) = %d,%d,%d (want blue)", b[0], b[1], b[2]);
	CHECK(near(mid, 128, 64, 64), "inside (1/4,1/4) = %d,%d,%d (want 128,64,64: interpolated)", mid[0], mid[1], mid[2]);

	if (ppm) {
		FILE *fp = fopen(ppm, "wb");
		if (fp) {
			fprintf(fp, "P6\n%d %d\n255\n", w, h);
			for (int y = h - 1; y >= 0; y--) {
				for (int x = 0; x < w; x++) {
					fwrite(PX(x, y), 1, 3, fp);
				}
			}
			fclose(fp);
			printf("INFO  wrote %s\n", ppm);
		}
	}
	OSMesaDestroyContext(ctx);
	printf("%s: %d failure(s), %s\n", g_fail ? "FAILED" : "ALL PASSED", g_fail, hw ? "on the GPU" : "in software");
	return g_fail;
}
