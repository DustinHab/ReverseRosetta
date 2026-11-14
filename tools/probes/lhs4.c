/* lhs4.c - double from Intel memory (byte-swapped) into the FPU, with dependent computation and a rotating scratch
   area: lwbrx/stw/lfd against lvx/vperm/stvx/lfd against prefetch (ring buffer, stored 4 iterations before).
   Build: gcc-4.2 -arch ppc -mcpu=7450 -maltivec -O2 -o lhs4 lhs4.c */
#include <stdio.h>
#include <sys/time.h>

#define N 20000000
static double now_sec(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec * 1e-6; }
static double src[64] __attribute__((aligned(16)));
static double store[16] __attribute__((aligned(16)));
static unsigned char swap[16] __attribute__((aligned(16))) = { 7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8 };

int main(void)
{
    double t0, base, h1 = 0, h2 = 0, h3 = 0;
    int i;
    for (i = 0; i < 64; i++) src[i] = i;
    t0 = now_sec();
    for (i = 0; i < N; i++) __asm__ volatile("" ::: "memory");
    base = (now_sec() - t0) / N * 1.333e9;
    t0 = now_sec();
    for (i = 0; i < N; i++) {
        double r;
        double *q = &src[i & 63], *z = &store[(i & 3) * 2];
        __asm__ volatile("li r0,4\n lwbrx r3,%2,r0\n lwbrx r4,0,%2\n stw r3,0(%3)\n stw r4,4(%3)\n lfd %0,0(%3)\n fadd %1,%1,%0"
                         : "=&f"(r), "+f"(h1) : "b"(q), "b"(z) : "r0", "r3", "r4", "memory");
    }
    printf("lwbrx stw lfd fadd               %5.1f cycles\n", (now_sec() - t0) / N * 1.333e9 - base);
    t0 = now_sec();
    __asm__ volatile("lvx v2,0,%0" : : "b"(swap) : "v2");
    for (i = 0; i < N; i++) {
        double r;
        double *q = &src[(i & 31) * 2], *z = &store[(i & 3) * 2];
        __asm__ volatile("lvx v0,0,%2\n vperm v1,v0,v0,v2\n stvx v1,0,%3\n lfd %0,0(%3)\n fadd %1,%1,%0"
                         : "=&f"(r), "+f"(h2) : "b"(q), "b"(z) : "v0", "v1", "memory");
    }
    printf("lvx vperm stvx lfd fadd          %5.1f cycles\n", (now_sec() - t0) / N * 1.333e9 - base);
    t0 = now_sec();
    for (i = 0; i < N; i++) {
        double r;
        double *q = &src[(i + 4) & 63], *z = &store[(i & 3) * 2];
        __asm__ volatile("lfd %0,0(%3)\n li r0,4\n lwbrx r3,%2,r0\n lwbrx r4,0,%2\n stw r3,0(%3)\n stw r4,4(%3)\n fadd %1,%1,%0"
                         : "=&f"(r), "+f"(h3) : "b"(q), "b"(z) : "r0", "r3", "r4", "memory");
    }
    printf("prefetch (ring, 4 ahead)         %5.1f cycles\n", (now_sec() - t0) / N * 1.333e9 - base);
    printf("(sums only to prevent optimizing away: %g %g %g)\n", h1, h2, h3);
    return 0;
}
