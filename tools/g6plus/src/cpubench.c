/* cpubench: cycles-per-iteration free estimate of the core's clock. A dependent chain of 3 one-cycle operations per
 * iteration (eor, lsr, add on a Cortex-A53 class core): iterations per second * 3 ~= the clock in Hz. Runs N threads. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void *run(void *arg) {
    uint64_t x = 1, n = 200000000ull; double t0 = now();
    for (uint64_t i = 0; i < n; i++) { x = (x ^ i) + (x >> 3); __asm__ volatile("" : "+r"(x)); }
    double dt = now() - t0;
    *(double *)arg = n * 3 / dt / 1e6;
    return (void *)(uintptr_t)x;
}
int main(int argc, char **argv) {
    int nt = argc > 1 ? atoi(argv[1]) : 1; pthread_t th[16]; double mhz[16];
    if (nt < 1 || nt > 16) nt = 1;
    for (int i = 0; i < nt; i++) pthread_create(&th[i], NULL, run, &mhz[i]);
    for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
    printf("%d thread(s): estimated core clock (MHz, 3 cycles per iteration assumed):", nt);
    for (int i = 0; i < nt; i++) printf(" %.0f", mhz[i]);
    printf("\n");
    return 0;
}
