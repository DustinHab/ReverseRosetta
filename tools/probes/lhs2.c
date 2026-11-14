/* lhs2.c - whether a time gap helps against the penalty of lfd after two stw. Also: paths without stw->lfd.
   Build: gcc-4.2 -arch ppc -mcpu=7450 -O2 -o lhs2 lhs2.c */
#include <stdio.h>
#include <sys/time.h>

#define N 20000000

static double now_sec(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec * 1e-6; }
static double pbuf[64] __attribute__((aligned(32)));
static double base;

#define RUN(name, loopbody) do { \
    double t0 = now_sec(), t; int i; \
    for (i = 0; i < N; i++) { __asm__ volatile(loopbody : : "b"(pbuf) : "r0", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "fr0", "fr1", "fr2", "memory"); } \
    t = (now_sec() - t0) / N * 1.333e9 - base; \
    printf("%-52s %5.1f cycles\n", name, t); \
} while (0)

#define MUL8 "mullw r5,r5,r6\n mullw r5,r5,r6\n mullw r5,r5,r6\n mullw r5,r5,r6\n mullw r5,r5,r6\n mullw r5,r5,r6\n mullw r5,r5,r6\n mullw r5,r5,r6\n"

int main(void)
{
    base = 0; RUN("empty (subtracted)", "");
    { double t0 = now_sec(); int i; for (i = 0; i < N; i++) __asm__ volatile(""); base = (now_sec() - t0) / N * 1.333e9; }
    RUN("8 dependent mullw", MUL8);
    RUN("stw stw, 8 dep. mullw, lfd", "stw r3,0(%0)\n stw r4,4(%0)\n" MUL8 "lfd f0,0(%0)");
    RUN("lfd (memory from previous iteration), stw stw", "lfd f0,0(%0)\n stw r3,0(%0)\n stw r4,4(%0)");
    RUN("lfd (previous iteration), 8 dep. mullw, stw stw", "lfd f0,0(%0)\n" MUL8 "stw r3,0(%0)\n stw r4,4(%0)");
    RUN("stw stw, 8 mullw, lfd + fadd chain", "stw r3,0(%0)\n stw r4,4(%0)\n" MUL8 "lfd f0,0(%0)\n fadd f1,f1,f0");
    RUN("stw stw lfd, ring buffer 4 iterations back", "addi r7,%0,0\n lwz r8,256(%0)\n addi r8,r8,8\n andi. r8,r8,24\n stw r8,256(%0)\n add r7,r7,r8\n stw r3,0(r7)\n stw r4,4(r7)\n xori r9,r8,16\n add r9,%0,r9\n lfd f0,0(r9)");
    RUN("same without lfd (ring buffer management alone)", "addi r7,%0,0\n lwz r8,256(%0)\n addi r8,r8,8\n andi. r8,r8,24\n stw r8,256(%0)\n add r7,r7,r8\n stw r3,0(r7)\n stw r4,4(r7)\n xori r9,r8,16\n add r9,%0,r9");
    RUN("lvx vperm stvx lfd (AltiVec swap)", "li r0,32\n lvx v0,%0,r0\n vperm v1,v0,v0,v0\n stvx v1,%0,r0\n lfd f0,32(%0)");
    RUN("stw stw lfd lfd lfd (read three times)", "stw r3,0(%0)\n stw r4,4(%0)\n lfd f0,0(%0)\n lfd f1,0(%0)\n lfd f2,0(%0)");
    RUN("lfs after stw (float)", "stw r3,0(%0)\n lfs f0,0(%0)");
    RUN("lfs lfs after stw stw (2 float)", "stw r3,0(%0)\n stw r4,4(%0)\n lfs f0,0(%0)\n lfs f1,4(%0)");
    RUN("stfd, lwz lwz (write-back)", "stfd f1,0(%0)\n lwz r3,0(%0)\n lwz r4,4(%0)");
    RUN("stfd, 8 dep. mullw, lwz lwz", "stfd f1,0(%0)\n" MUL8 "lwz r3,0(%0)\n lwz r4,4(%0)");
    return 0;
}
