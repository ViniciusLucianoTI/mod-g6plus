/*
 * osmesa_lines: GL_LINES spans on the phone (derived from osmesa_tri). An offscreen OpenGL ES 2 context through OSMesa
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
		OSMESA_PROFILE, 0x36 /* Bergamota Mesa patch: OpenGL ES profile */, OSMESA_CONTEXT_MAJOR_VERSION, 2, OSMESA_CONTEXT_MINOR_VERSION, 0, 0
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
	int hw = renderer && strstr(renderer, "FD") != NULL;

	/* horizontal one-pixel spans, as CoreAnimation's CA::CG renderer draws glyphs and shapes (GL_LINES, hundreds of
	 * vertices per call): rows 40..199, every row a span from x=30 to x=30+len at pixel centres, black on white. */
	static const char *vs = "attribute vec2 p; uniform vec2 size; void main() { gl_Position = vec4(p / size * 2.0 - 1.0, 0.0, 1.0); }";
	static const char *fs = "precision mediump float; void main() { gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0); }";
	GLuint prog = glCreateProgram();
	glAttachShader(prog, shader(GL_VERTEX_SHADER, vs));
	glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(prog, 0, "p");
	glLinkProgram(prog);
	glUseProgram(prog);
	glUniform2f(glGetUniformLocation(prog, "size"), (GLfloat)w, (GLfloat)h);
	static GLfloat v[160 * 4];
	long want = 0;
	for (int r = 0; r < 160; r++) {
		int len = 10 + r;
		v[r * 4] = 30; v[r * 4 + 1] = 40 + r + 0.5f; v[r * 4 + 2] = 30 + len; v[r * 4 + 3] = 40 + r + 0.5f;
		want += len;
	}
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v);
	glEnableVertexAttribArray(0);
	glViewport(0, 0, w, h);
	glClearColor(1, 1, 1, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_LINES, 0, 320);
	glFinish();
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
#define PX(x, y) (px + 4 * ((size_t)(y) * w + (x)))
	long black = 0, rows = 0, outside = 0;
	for (int y = 0; y < h; y++) {
		int n = 0;
		for (int x = 0; x < w; x++) {
			if (PX(x, y)[0] < 128) { n++; if (y < 40 || y >= 200 || x < 29 || x > 30 + 10 + (y - 40) + 1) outside++; }
		}
		black += n; rows += n > 0;
	}
	CHECK(labs(black - want) <= 160 && rows == 160 && outside == 0, "spans: %ld black pixels (want %ld +-160) in %ld rows (want 160), %ld outside the wedge", black, want, rows, outside);
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
