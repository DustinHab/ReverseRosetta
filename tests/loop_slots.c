/* loop_slots.c - a loop that keeps seven variables on the stack (built with -O0) and reads them in every iteration:
 * the translator holds the most used ones in registers (r28..r31). An MMX instruction in the loop goes through a C
 * call in the middle of the block; a call to a function in the loop leaves the slots dormant. i386 only: comparison
 * translated against interpreter. */
#include <stdio.h>

static unsigned int side(unsigned int x) { return x * 2654435761u >> 7; }

static unsigned int work(unsigned int n, int mmx, int call)
{
    unsigned int a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, i;
    for (i = 0; i < n; i++) {
        a += b ^ i;
        b += c;
        c ^= d + i;
        d += e;
        e += f ^ a;
        f += g;
        g ^= a + b;
        if (mmx) __asm__ volatile("paddw %%mm0, %%mm1" : : : "mm0", "mm1");
        if (call) g += side(i);
    }
    if (mmx) __asm__ volatile("emms");
    return a ^ (b << 1) ^ (c << 2) ^ (d << 3) ^ (e << 4) ^ (f << 5) ^ (g << 6);
}

int main(void)
{
    unsigned int r;
    for (r = 0; r < 3; r++)
        printf("%u: plain %08x, mmx %08x, call %08x\n", r, work(1000 + r, 0, 0), work(1000 + r, 1, 0), work(1000 + r, 0, 1));
    return 0;
}
