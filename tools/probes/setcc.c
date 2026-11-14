/* setcc.c - condition as 0/1 on the G4: li/bc/li against mfcr/rlwinm, with random and with constant
   compares. Build: gcc-4.2 -arch ppc -mcpu=7450 -O2 -o setcc setcc.c */
#include <stdio.h>
#include <sys/time.h>

#define N 20000000
static double now_sec(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec * 1e-6; }
static int vals[4096];

int main(void)
{
    double t0, base;
    unsigned x = 12345, s1 = 0, s2 = 0;
    int i, kind;
    for (i = 0; i < 4096; i++) { x = x * 1103515245u + 12345u; vals[i] = (int)(x >> 8) & 0xffff; }
    t0 = now_sec();
    for (i = 0; i < N; i++) __asm__ volatile("" ::: "memory");
    base = (now_sec() - t0) / N * 1.333e9;
    for (kind = 0; kind < 2; kind++) {
        int msk = kind ? 0 : 4095;         /* 0: random values, 1: always the same value */
        t0 = now_sec();
        for (i = 0; i < N; i++) {
            unsigned r;
            __asm__ volatile("cmpwi %1,0x7fff\n li %0,1\n bgt 1f\n li %0,0\n1:" : "=&r"(r) : "r"(vals[i & msk]) : "cr0");
            s1 += r;
        }
        printf("%s li/bgt/li      %5.1f cycles\n", kind ? "same:     " : "random:   ", (now_sec() - t0) / N * 1.333e9 - base);
        t0 = now_sec();
        for (i = 0; i < N; i++) {
            unsigned r;
            __asm__ volatile("cmpwi %1,0x7fff\n mfcr %0\n rlwinm %0,%0,2,31,31" : "=r"(r) : "r"(vals[i & msk]) : "cr0");
            s2 += r;
        }
        printf("%s mfcr/rlwinm    %5.1f cycles\n", kind ? "same:     " : "random:   ", (now_sec() - t0) / N * 1.333e9 - base);
    }
    printf("(%u %u)\n", s1, s2);
    return 0;
}
