/*
 * osmesa_rtt: render to texture in two visits on the phone (derived from osmesa_tri). An offscreen OpenGL ES 2 context through OSMesa
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

	/* Render to a texture in two visits, with another framebuffer in between, as the render server does for a group
	 * (clock, tab bar glyphs): what the first visit drew must still be there after the second. */
	static const char *vs = "attribute vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }";
	static const char *fs = "precision mediump float; uniform vec4 c; void main() { gl_FragColor = c; }";
	GLuint prog = glCreateProgram();
	glAttachShader(prog, shader(GL_VERTEX_SHADER, vs));
	glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(prog, 0, "p");
	glLinkProgram(prog);
	glUseProgram(prog);
	GLint uc = glGetUniformLocation(prog, "c");
	static const GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glEnableVertexAttribArray(0);
	GLuint tex[2], fbo[2];
	glGenTextures(2, tex); glGenFramebuffers(2, fbo);
	int tw[2] = { w, 64 }, th[2] = { h, 64 };
	for (int i = 0; i < 2; i++) {
		glBindTexture(GL_TEXTURE_2D, tex[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw[i], th[i], 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[i], 0);
	}
	CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "framebuffers complete");
	glEnable(GL_SCISSOR_TEST);
	/* visit 1: clear to blue, left half red */
	glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]); glViewport(0, 0, w, h);
	glScissor(0, 0, w, h); glClearColor(0, 0, 1, 1); glClear(GL_COLOR_BUFFER_BIT);
	glScissor(0, 0, w / 2, h); glUniform4f(uc, 1, 0, 0, 1); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	/* something else in between */
	glBindFramebuffer(GL_FRAMEBUFFER, fbo[1]); glViewport(0, 0, 64, 64);
	glScissor(0, 0, 64, 64); glUniform4f(uc, 1, 1, 1, 1); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	/* visit 2: no clear, a green band in the top right quarter */
	glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]); glViewport(0, 0, w, h);
	glScissor(w / 2, h / 2, w / 2, h / 2); glUniform4f(uc, 0, 1, 0, 1); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisable(GL_SCISSOR_TEST);
	glFinish();
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
#define PX(x, y) (px + 4 * ((size_t)(y) * w + (x)))
	const unsigned char *a = PX(w / 4, h / 4), *b = PX(w / 4, 3 * h / 4), *c = PX(3 * w / 4, h / 4), *d = PX(3 * w / 4, 3 * h / 4);
	CHECK(a[0] > 200 && a[1] < 50 && a[2] < 50, "left bottom %d,%d,%d (want red, from visit 1)", a[0], a[1], a[2]);
	CHECK(b[0] > 200 && b[1] < 50 && b[2] < 50, "left top %d,%d,%d (want red, from visit 1)", b[0], b[1], b[2]);
	CHECK(c[2] > 200 && c[0] < 50 && c[1] < 50, "right bottom %d,%d,%d (want blue, the clear of visit 1)", c[0], c[1], c[2]);
	CHECK(d[1] > 200 && d[0] < 50 && d[2] < 50, "right top %d,%d,%d (want green, from visit 2)", d[0], d[1], d[2]);
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
