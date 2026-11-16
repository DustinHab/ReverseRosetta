/* cond_sizes.c - conditions of cmp and test on 8-, 16- and 32-bit operands, read in another block: a jump between
 * the compare and the setcc makes the translator keep the flags in memory (kind unknown at translation
 * time), so the condition is computed from the stored operands (cond_helper). All 16 conditions, border values.
 * Comparison translated against interpreter (ROSETTA_THRESHOLD=1 against ROSETTA_INTERP=1) and against an x86 PC
 * (the same source as x86-64). */
#include <stdio.h>

/* two conditions after OP (on dl/dx/edx = a and cl/cx/ecx = b), read with setcc in another block: both conditional
   jumps lead past 60 nops, further than a block reaches, so the flags cross a block boundary as they do between
   gcc's cmpb and the seta at the jump target (bzip2's mainGtU). Bits SH and SH + 1. Fixed registers: i386 PIC code
   has too few byte registers for the allocator. (Flags live across an indirect jump are not supported: compilers
   never leave them so.) */
#define C2(OP, S1, S2, SH)                                                                                        \
    __asm__ volatile("movl %1, %%edx\n\tmovl %2, %%ecx\n\t" OP "\n\tjne 3f\n\tje 3f\n\t.space 60, 0x90\n" \
                     "3:\t" S1 " %b0\n\t" S2 " %h0" : "=&a"(x) : "m"(a), "m"(b) : "cc", "ecx", "edx");           \
    m |= ((x & 1) | (x >> 7 & 2)) << (SH);

#define CONDS(OP)                                                                                                 \
    m = 0;                                                                                                       \
    C2(OP, "seto", "setno", 0) C2(OP, "setb", "setae", 2) C2(OP, "sete", "setne", 4)                             \
    C2(OP, "setbe", "seta", 6) C2(OP, "sets", "setns", 8) C2(OP, "setp", "setnp", 10)                            \
    C2(OP, "setl", "setge", 12) C2(OP, "setle", "setg", 14)

int main(void)
{
    static const unsigned int v[] = { 0, 1, 2, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000,
                                      0x7fffffff, 0x80000000u, 0xfffffffeu, 0xffffffffu, 0x12345678, 0x9abcdef0u };
    const int n = sizeof v / sizeof v[0];
    unsigned int sum = 0, round;
    int i, j;
    for (round = 0; round < 3; round++)
        for (i = 0; i < n; i++)
            for (j = 0; j < n; j++) {
                unsigned int a = v[i] ^ (round * 0x01010101u), b = v[j], m;
                unsigned long x;
                CONDS("cmpb %%cl, %%dl")
                sum = sum * 33 + m;
                if (round == 2 && j == 3) printf("cmpb %08x %08x: %04x\n", a, b, m);
                CONDS("cmpw %%cx, %%dx")
                sum = sum * 33 + m;
                if (round == 2 && j == 5) printf("cmpw %08x %08x: %04x\n", a, b, m);
                CONDS("cmpl %%ecx, %%edx")
                sum = sum * 33 + m;
                CONDS("testb %%cl, %%dl")
                sum = sum * 33 + m;
                CONDS("testw %%cx, %%dx")
                sum = sum * 33 + m;
                CONDS("testl %%ecx, %%edx")
                sum = sum * 33 + m;
            }
    printf("sum %08x\n", sum);
    return 0;
}
