/* bdnz.c - whether a backward branch with CTR counting and condition (bdnzf) costs more on the G4 than a plain bc.
   gcc -O2 -o bdnz bdnz.c && ./bdnz */
#include <stdio.h>
#include <sys/time.h>

static double now(void)
{
    struct timeval t;
    gettimeofday(&t, 0);
    return t.tv_sec + t.tv_usec * 1e-6;
}

int main(void)
{
    unsigned n = 200000000;
    double t0;
    int r;
    for (r = 0; r < 2; r++) {
        t0 = now();
        __asm__ volatile(
            "mr r3,%0\n"
            "li r4,0\n"
            "1: addi r4,r4,1\n"
            "addi r6,r6,3\n"
            "cmpw cr0,r4,r3\n"
            "bc 4,2,1b\n" : : "r"(n) : "r3", "r4", "r6", "cr0");
        printf("bne          %.3f ns per iteration\n", (now() - t0) * 1e9 / n);
        t0 = now();
        __asm__ volatile(
            "mr r3,%0\n"
            "li r4,0\n"
            "lis r5,0x7fff\n"
            "mtctr r5\n"
            "1: addi r4,r4,1\n"
            "addi r6,r6,3\n"
            "cmpw cr0,r4,r3\n"
            "bc 0,2,1b\n"
            "bc 4,2,1b\n" : : "r"(n) : "r3", "r4", "r5", "r6", "cr0", "ctr");
        printf("bdnzf+bne    %.3f ns per iteration\n", (now() - t0) * 1e9 / n);
        t0 = now();
        __asm__ volatile(
            "lis r5,0x0bec\n"
            "ori r5,r5,0x2000\n"
            "mtctr r5\n"
            "1: addi r6,r6,3\n"
            "addi r7,r7,1\n"
            "bdnz 1b\n" : : : "r5", "r6", "r7", "ctr");
        printf("bdnz         %.3f ns per iteration\n", (now() - t0) * 1e9 / n);
        t0 = now();
        __asm__ volatile(
            "lis r3,0x0bec\n"
            "ori r3,r3,0x2000\n"
            "li r4,0\n"
            "1: addi r6,r6,3\n"
            "addi r4,r4,1\n"
            "cmplw cr1,r4,r3\n"
            "bc 12,4,1b\n" : : : "r3", "r4", "r6", "cr1");
        printf("blt cr1      %.3f ns per iteration\n", (now() - t0) * 1e9 / n);
    }
    return 0;
}
