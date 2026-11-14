/* lhs1.c - cost of load-hit-store on the G4 (7447A): lfd after two stw, lwz after stfd, with and without distance.
   Build: gcc-4.2 -arch ppc -mcpu=7450 -O2 -o lhs lhs1.c */
#include <stdio.h>
#include <sys/time.h>

#define N 20000000

static double now_sec(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec * 1e-6; }
static double pbuf[64] __attribute__((aligned(32)));

#define RUN(name, loopbody) do { \
    double t0 = now_sec(); int i; \
    for (i = 0; i < N; i++) { __asm__ volatile(loopbody : : "b"(pbuf) : "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "fr0", "fr1", "memory"); } \
    printf("%-44s %5.2f ns  %4.1f cycles\n", name, (now_sec() - t0) / N * 1e9, (now_sec() - t0) / N * 1.333e9); \
} while (0)

int main(void)
{
    RUN("empty", "");
    RUN("lfd (old) + fadd", "lfd f0,64(%0)\n fadd f1,f1,f0");
    RUN("stw stw lfd + fadd (same address)", "stw r3,0(%0)\n stw r4,4(%0)\n lfd f0,0(%0)\n fadd f1,f1,f0");
    RUN("stw stw lfd (without fadd)", "stw r3,0(%0)\n stw r4,4(%0)\n lfd f0,0(%0)");
    RUN("stw stw, 6 integer instructions, lfd", "stw r3,0(%0)\n stw r4,4(%0)\n addi r5,r5,1\n addi r6,r6,1\n addi r7,r7,1\n addi r8,r8,1\n addi r9,r9,1\n addi r10,r10,1\n lfd f0,0(%0)");
    RUN("stw stw, 12 integer instructions, lfd", "stw r3,0(%0)\n stw r4,4(%0)\n addi r5,r5,1\n addi r6,r6,1\n addi r7,r7,1\n addi r8,r8,1\n addi r9,r9,1\n addi r10,r10,1\n addi r5,r5,1\n addi r6,r6,1\n addi r7,r7,1\n addi r8,r8,1\n addi r9,r9,1\n addi r10,r10,1\n lfd f0,0(%0)");
    RUN("2x (stw stw) then 2x lfd", "stw r3,0(%0)\n stw r4,4(%0)\n stw r3,8(%0)\n stw r4,12(%0)\n lfd f0,0(%0)\n lfd f1,8(%0)");
    RUN("lwbrx lwbrx stw stw lfd (as in the JIT)", "li r0,4\n lwbrx r3,%0,r0\n lwbrx r4,0,%0\n stw r3,16(%0)\n stw r4,20(%0)\n lfd f0,16(%0)");
    RUN("stfd lwz lwz", "stfd f1,0(%0)\n lwz r3,0(%0)\n lwz r4,4(%0)");
    RUN("stfd lwz lwz stwbrx stwbrx (as in the JIT)", "stfd f1,0(%0)\n lwz r3,0(%0)\n lwz r4,4(%0)\n stwbrx r4,0,%0\n addi r5,%0,4\n stwbrx r3,0,r5");
    RUN("stw lfs (same size)", "stw r3,0(%0)\n lfs f0,0(%0)");
    RUN("stfd lfd (same size)", "stfd f1,0(%0)\n lfd f0,0(%0)");
    RUN("stw lwz (same size)", "stw r3,0(%0)\n lwz r5,0(%0)");
    RUN("stwbrx lwbrx (same size)", "stwbrx r3,0,%0\n lwbrx r5,0,%0");
    RUN("stw stw lwz lwz (copy via integer)", "stw r3,0(%0)\n stw r4,4(%0)\n lwz r5,0(%0)\n lwz r6,4(%0)");
    return 0;
}
