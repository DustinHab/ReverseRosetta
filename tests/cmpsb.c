/* cmpsb.c - cmpsb with repz/repnz/without, forward and backward (DF), ecx = 0: pointers, counter and flags
 * afterwards. i386 only (inline assembler). Comparison: rosetta against ROSETTA_NO_CMPSB=1 and ROSETTA_INTERP=1. */
#include <stdio.h>

static const char a[] = "__DATA\0__dyld\0__text\0abcdefgh";
static const char b[] = "__DATA\0__dyle\0__TEXT\0abcdxfgh";

static void probe(const char *name, int kind, unsigned before, const char *s, const char *t, unsigned n, int ret)
{
    unsigned esi = (unsigned)s, edi = (unsigned)t, ecx = n, fl, in = before | (ret ? 0x400 : 0);
    if (kind == 1)
        __asm__ volatile("pushl %4\n\tpopfl\n\trepz cmpsb\n\tpushfl\n\tpopl %3\n\tcld"
                         : "+S"(esi), "+D"(edi), "+c"(ecx), "=r"(fl) : "r"(in) : "memory", "cc");
    else if (kind == 2)
        __asm__ volatile("pushl %4\n\tpopfl\n\trepnz cmpsb\n\tpushfl\n\tpopl %3\n\tcld"
                         : "+S"(esi), "+D"(edi), "+c"(ecx), "=r"(fl) : "r"(in) : "memory", "cc");
    else
        __asm__ volatile("pushl %4\n\tpopfl\n\tcmpsb\n\tpushfl\n\tpopl %3\n\tcld"
                         : "+S"(esi), "+D"(edi), "+c"(ecx), "=r"(fl) : "r"(in) : "memory", "cc");
    printf("%-22s esi%+d edi%+d ecx=%u flags=%03x\n", name, (int)(esi - (unsigned)s), (int)(edi - (unsigned)t), ecx,
           fl & 0x8d5);
}

int main(void)
{
    int i;
    for (i = 0; i < 3; i++) {               /* several times: cold, then translated */
        probe("repz equal", 1, 0x202, a, b, 7, 0);
        probe("repz not equal", 1, 0x202, a + 7, b + 7, 7, 0);
        probe("repz upper case", 1, 0x202, a + 14, b + 14, 7, 0);
        probe("repz ecx=0 ZF", 1, 0x242, a + 7, b + 7, 0, 0);
        probe("repz ecx=0 CF", 1, 0x203, a + 7, b + 7, 0, 0);
        probe("repnz", 2, 0x202, a + 21, b + 21, 8, 0);
        probe("repnz equal at end", 2, 0x202, a, b + 7, 6, 0);
        probe("single equal", 0, 0x202, a, b, 1, 0);
        probe("single less", 0, 0x202, a + 14, b + 14, 1, 0);
        probe("single greater", 0, 0x202, a + 16, b + 16, 1, 0);
        probe("repz backward", 1, 0x202, a + 28, b + 28, 8, 1);
        probe("repz backward eq.", 1, 0x202, a + 5, b + 5, 6, 1);
    }
    return 0;
}
