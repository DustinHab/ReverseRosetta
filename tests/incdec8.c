/* incdec8.c - incb/decb in byte registers (al, ah) and in memory, with the flags they set (OF SF ZF AF PF) and the
 * carry they keep, and as the counter of a loop (jnz after decb). i386 only: comparison translated against
 * interpreter (ROSETTA_THRESHOLD=1 against ROSETTA_INTERP=1). */
#include <stdio.h>

int main(void)
{
    static unsigned char mem[6] = { 0x7f, 0x80, 0xff, 0x00, 0x0f, 0x10 };
    unsigned int k, f, v, round;
    for (round = 0; round < 3; round++)
        for (k = 0; k < 6; k++) {
            v = 0x11223300u | mem[k];
            __asm__ volatile("stc\n\tincb %%al\n\tpushfl\n\tpopl %0" : "=r"(f), "+a"(v) : : "cc");
            if (round == 2) printf("incb al: %08x flags %03x\n", v, f & 0x8d5);
            v = 0x1122ff44u ^ ((unsigned)mem[k] << 8);
            __asm__ volatile("clc\n\tdecb %%ah\n\tpushfl\n\tpopl %0" : "=r"(f), "+a"(v) : : "cc");
            if (round == 2) printf("decb ah: %08x flags %03x\n", v, f & 0x8d5);
            __asm__ volatile("stc\n\tdecb %1\n\tpushfl\n\tpopl %0" : "=r"(f), "+m"(mem[k]) : : "cc");
            if (round == 2) printf("decb m: %02x flags %03x\n", mem[k], f & 0x8d5);
            __asm__ volatile("clc\n\tincb %1\n\tincb %1\n\tpushfl\n\tpopl %0" : "=r"(f), "+m"(mem[k]) : : "cc");
            if (round == 2) printf("incb m twice: %02x flags %03x\n", mem[k], f & 0x8d5);
        }
    for (k = 0, v = 0; k < 600; k++) {      /* byte counter as the loop condition */
        unsigned char c = (unsigned char)k;
        __asm__ volatile("1: incl %0\n\tdecb %1\n\tjnz 1b" : "+r"(v), "+q"(c) : : "cc");
    }
    printf("decb loop: %u\n", v);
    return 0;
}
