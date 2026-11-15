/* mem16.c - 16-bit and byte operations on memory that real programs use in hot loops: incw/decw (gzip's frequency
 * counters, sqlite), shlb/shrb/sarb of a byte in memory (gzip's flag byte), shlw/shrw/sarw of a word in memory by
 * fixed counts, movw $imm16 into a 16-bit register (the upper half must stay), movw to and from a fixed address
 * (gzip's bit buffer), with the flags they set (CF OF SF
 * ZF PF; AF and the OF of multi-bit shifts are undefined on Intel and left out). i386 only: comparison translated
 * against interpreter (ROSETTA_THRESHOLD=1 against ROSETTA_INTERP=1). */
#include <stdio.h>

#ifdef __x86_64__                           /* the same instructions, for a reference on an x86-64 PC */
#define POPF "\n\tpushfq\n\tpopq %0"
#else
#define POPF "\n\tpushfl\n\tpopl %0"
#endif
#define F(f) ((f) & 0x8c5)                  /* OF SF ZF PF CF */
unsigned short g16 = 0x1111;               /* movw %ax, g16 and back: the moffs forms a3/a1 */
#define FS(f) ((f) & 0x0c5)                 /* SF ZF PF CF */

int main(void)
{
    static unsigned short w[6] = { 0x7fff, 0x8000, 0xffff, 0x0000, 0x00ff, 0x1234 };
    static unsigned char m[6] = { 0x81, 0x80, 0xff, 0x00, 0x0f, 0x7e };
    static unsigned short freq[256];
    unsigned long f;
    unsigned int k, v, round, sum = 0;
    for (round = 0; round < 3; round++)
        for (k = 0; k < 6; k++) {
            unsigned short a = w[k], b = w[k];
            unsigned char c = m[k];
            __asm__ volatile("stc\n\tincw %1" POPF : "=r"(f), "+m"(a) : : "cc");
            if (round == 2) printf("incw m: %04x flags %03x\n", a, (unsigned)F(f));
            __asm__ volatile("clc\n\tdecw %1\n\tdecw %1" POPF : "=r"(f), "+m"(b) : : "cc");
            if (round == 2) printf("decw m twice: %04x flags %03x\n", b, (unsigned)F(f));
            a = w[k];
            __asm__ volatile("shlw $1, %1" POPF : "=r"(f), "+m"(a) : : "cc");
            if (round == 2) printf("shlw 1: %04x flags %03x\n", a, (unsigned)F(f));
            a = w[k];
            __asm__ volatile("shrw $3, %1" POPF : "=r"(f), "+m"(a) : : "cc");
            if (round == 2) printf("shrw 3: %04x flags %03x\n", a, (unsigned)FS(f));
            a = w[k];
            __asm__ volatile("sarw $5, %1" POPF : "=r"(f), "+m"(a) : : "cc");
            if (round == 2) printf("sarw 5: %04x flags %03x\n", a, (unsigned)FS(f));
            a = w[k];
            __asm__ volatile("shlw $17, %1" POPF : "=r"(f), "+m"(a) : : "cc");
            if (round == 2) printf("shlw 17: %04x flags %03x\n", a, (unsigned)FS(f) & ~1u);
            __asm__ volatile("shrb %1" POPF : "=r"(f), "+m"(c) : : "cc");
            if (round == 2) printf("shrb 1: %02x flags %03x\n", c, (unsigned)F(f));
            c = m[k];
            __asm__ volatile("shlb $3, %1" POPF : "=r"(f), "+m"(c) : : "cc");
            if (round == 2) printf("shlb 3: %02x flags %03x\n", c, (unsigned)FS(f));
            c = m[k];
            __asm__ volatile("sarb $2, %1" POPF : "=r"(f), "+m"(c) : : "cc");
            if (round == 2) printf("sarb 2: %02x flags %03x\n", c, (unsigned)FS(f));
            v = 0xa5a50000u | k;
            __asm__ volatile("movw $0xbeef, %w0" : "+r"(v));
            if (round == 2) printf("movw imm: %08x\n", v);
        }
    for (k = 0; k < 100000; k++) {          /* counters in a loop, as gzip counts literals */
        unsigned int c = (k * 2654435761u) >> 24;
        __asm__ volatile("incw %0" : "+m"(freq[c]) : : "cc");
    }
    for (k = 0; k < 256; k++) sum = sum * 31 + freq[k];
    printf("frequencies: %08x\n", sum);
    {
        unsigned char flags = 0x5a;
        for (k = 0; k < 1000; k++) {        /* the flag byte shifted once per symbol */
            __asm__ volatile("shrb %0" : "+m"(flags) : : "cc");
            if (!(k & 7)) flags |= (unsigned char)(k << 3);
            sum += flags;
        }
    }
    printf("flag byte: %08x\n", sum);
    for (k = 0; k < 2000; k++) {            /* bi_buf |= value << bi_valid; a 16-bit global written and read back */
        unsigned int x = 0xffff0000u | k, y = 0;
#ifdef __x86_64__
        g16 = (unsigned short)x; y = 0x5a5a0000u | g16;
#else
        y = 0x5a5a0000u;
        __asm__ volatile("movw %%ax, _g16" : : "a"(x) : "memory");
        __asm__ volatile("movw _g16, %%ax" : "+a"(y) : : "memory");
#endif
        sum = sum * 33 + y;
    }
    printf("fixed address: %08x %04x\n", sum, g16);
    return 0;
}
