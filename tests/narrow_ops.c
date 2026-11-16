/* narrow_ops.c - or/and/xor of 8- and 16-bit registers (al, ah, dl, dh, ax, dx) with constants, flags dead
 * afterwards (the translator then works on the whole host register) and live (a jcc reads them). i386 only:
 * comparison translated against interpreter (ROSETTA_THRESHOLD=1 against ROSETTA_INTERP=1). */
#include <stdio.h>

static unsigned int dead(unsigned int a, unsigned int d)
{
    __asm__ volatile("orb $0xff, %%dl\n\t"
                     "xorb $0x5a, %%ah\n\t"
                     "andb $0xf0, %%al\n\t"
                     "andb $0x81, %%dh\n\t"
                     "orw $0x1001, %%ax\n\t"
                     "xorw $0x8080, %%dx\n\t"
                     "andw $0x00ff, %%dx\n\t"
                     "andb $0x3c, %%al\n\t"
                     "andw $0xf00f, %%ax\n\t"
                     "orb $0x80, %%dh\n\t"
                     "xorb $0x01, %%al\n\t"
                     "addl %%edx, %%eax"
                     : "+a"(a), "+d"(d) : : "cc");
    return a;
}

static unsigned int live(unsigned int a)
{
    unsigned int r = 0;
    __asm__ volatile("andb $0x0f, %%al\n\t"
                     "jz 1f\n\t"
                     "orl $0x100, %0\n"
                     "1:\txorb $0x80, %%ah\n\t"
                     "js 2f\n\t"
                     "orl $0x200, %0\n"
                     "2:\torw $0x0101, %%ax\n\t"
                     "jp 3f\n\t"
                     "orl $0x400, %0\n"
                     "3:"
                     : "+r"(r), "+a"(a) : : "cc");
    return r ^ a;
}

int main(void)
{
    static const unsigned int v[] = { 0, 0xffffffffu, 0x12345678u, 0x80808080u, 0x0f0f0f0fu, 0xdeadbeefu, 0x00ff00ffu };
    unsigned int i, j, round, s = 0;
    for (round = 0; round < 3; round++)
        for (i = 0; i < sizeof v / sizeof v[0]; i++)
            for (j = 0; j < sizeof v / sizeof v[0]; j++) {
                unsigned int x = dead(v[i], v[j]), y = live(v[i] ^ v[j]);
                if (round == 2 && j == i) printf("%08x %08x: dead %08x live %08x\n", v[i], v[j], x, y);
                s = s * 31 + x + y;
            }
    printf("sum %08x\n", s);
    return 0;
}
