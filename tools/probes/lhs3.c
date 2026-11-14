/* lhs3.c - cvtsi2sd on the G4: previous path (stw stw lfd, magic 2^52+2^31) against two float halves (stw lfs).
   The scratch area rotates over 8 slots as in the translator. Build: gcc-4.2 -arch ppc -mcpu=7450 -O2 -o lhs3 lhs3.c */
#include <stdio.h>
#include <sys/time.h>

#define N 20000000
static double now_sec(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec * 1e-6; }
static double pbuf[16] __attribute__((aligned(32)));
static const double k[3] = { 4503601774854144.0, 65536.0, 551911686144.0 };   /* 2^52+2^31, 2^16, 2^39+2^31+2^23 */

int main(void)
{
    double t0, base, h1 = 0, h2 = 0;
    int i;
    register double m asm("fr22") = k[0], c16 asm("fr23") = k[1], kk asm("fr24") = k[2];
    t0 = now_sec();
    for (i = 0; i < N; i++) __asm__ volatile("" ::: "memory");
    base = (now_sec() - t0) / N * 1.333e9;
    t0 = now_sec();
    for (i = 0; i < N; i++) {
        double r;
        double *z = &pbuf[i & 7];
        __asm__ volatile("xoris r0,%2,0x8000\n stw r0,4(%3)\n lis r0,0x4330\n stw r0,0(%3)\n lfd %0,0(%3)\n fsub %0,%0,%4\n fadd %1,%1,%0"
                         : "=&f"(r), "+f"(h1) : "r"(i * 7 - 1000), "b"(z), "f"(m) : "r0", "memory");
    }
    printf("stw stw lfd fsub (previous)      %5.1f cycles\n", (now_sec() - t0) / N * 1.333e9 - base);
    t0 = now_sec();
    for (i = 0; i < N; i++) {
        double r, s;
        double *z = &pbuf[i & 7];
        __asm__ volatile("rlwinm r3,%3,0,16,31\n oris r3,r3,0x4b00\n rlwinm r4,%3,16,16,31\n xori r4,r4,0x8000\n oris r4,r4,0x4b00\n"
                         " stw r3,0(%4)\n stw r4,4(%4)\n lfs %1,4(%4)\n lfs %0,0(%4)\n fmadd %0,%1,%5,%0\n fsub %0,%0,%6\n fadd %2,%2,%0"
                         : "=&f"(r), "=&f"(s), "+f"(h2) : "r"(i * 7 - 1000), "b"(z), "f"(c16), "f"(kk) : "r3", "r4", "memory");
    }
    printf("2x (stw lfs) fmadd fsub (new)    %5.1f cycles\n", (now_sec() - t0) / N * 1.333e9 - base);
    printf("sums %.17g %.17g (%s)\n", h1, h2, h1 == h2 ? "equal" : "DIFFERENT");
    return 0;
}
