/* lhs5.c - general AltiVec path (any address: two lvx, swap pattern from a table by a & 15) and copies
   (value from the storage slot back to memory via lwz). Build: gcc-4.2 -arch ppc -mcpu=7450 -maltivec -O2 */
#include <stdio.h>
#include <sys/time.h>

#define N 20000000
static double now_sec(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec * 1e-6; }
static double src[64] __attribute__((aligned(16)));
static double target[64] __attribute__((aligned(16)));
static double store[16] __attribute__((aligned(16)));
static unsigned char tab[16][16] __attribute__((aligned(16)));

int main(void)
{
    double t0, base, h1 = 0, h2 = 0;
    int i, o, j;
    for (o = 0; o < 16; o++) for (j = 0; j < 16; j++) tab[o][j] = (unsigned char)(j < 8 ? o + 7 - j : o + 7);
    t0 = now_sec();
    for (i = 0; i < N; i++) __asm__ volatile("" ::: "memory");
    base = (now_sec() - t0) / N * 1.333e9;
    t0 = now_sec();
    for (i = 0; i < N; i++) {
        double r;
        double *q = &src[i & 63], *z = &store[(i & 3) * 2];
        __asm__ volatile("li r0,7\n lvx v0,0,%2\n lvx v1,%2,r0\n rlwinm r0,%2,4,24,27\n lvx v2,%4,r0\n vperm v3,v0,v1,v2\n"
                         " stvx v3,0,%3\n lfd %0,0(%3)\n fadd %1,%1,%0"
                         : "=&f"(r), "+f"(h1) : "b"(q), "b"(z), "b"(tab) : "r0", "v0", "v1", "v2", "v3", "memory");
    }
    printf("general: 2x lvx, table, vperm, stvx, lfd, fadd    %5.1f cycles\n", (now_sec() - t0) / N * 1.333e9 - base);
    t0 = now_sec();
    for (i = 0; i < N; i++) {
        double *q = &src[i & 63], *z = &store[(i & 3) * 2], *d = &target[i & 63];
        __asm__ volatile("li r0,7\n lvx v0,0,%0\n lvx v1,%0,r0\n rlwinm r0,%0,4,24,27\n lvx v2,%3,r0\n vperm v3,v0,v1,v2\n"
                         " stvx v3,0,%1\n lwz r3,0(%1)\n lwz r4,4(%1)\n stwbrx r4,0,%2\n li r0,4\n stwbrx r3,%2,r0"
                         : : "b"(q), "b"(z), "b"(d), "b"(tab) : "r0", "r3", "r4", "v0", "v1", "v2", "v3", "memory");
    }
    printf("copy via AltiVec slot (stvx, lwz, stwbrx)         %5.1f cycles\n", (now_sec() - t0) / N * 1.333e9 - base);
    t0 = now_sec();
    for (i = 0; i < N; i++) {
        double *q = &src[i & 63], *z = &store[(i & 3) * 2], *d = &target[i & 63];
        __asm__ volatile("li r0,4\n lwbrx r3,%0,r0\n lwbrx r4,0,%0\n stw r3,0(%1)\n stw r4,4(%1)\n lwz r3,0(%1)\n lwz r4,4(%1)\n"
                         " stwbrx r4,0,%2\n stwbrx r3,%2,r0"
                         : : "b"(q), "b"(z), "b"(d) : "r0", "r3", "r4", "memory");
    }
    printf("copy via integer slot (current)                   %5.1f cycles\n", (now_sec() - t0) / N * 1.333e9 - base);
    printf("(%g %g)\n", h1, h2);
    return 0;
}
