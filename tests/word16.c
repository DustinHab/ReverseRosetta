/* word16.c - arithmetic with 16-bit words in memory and in registers (orw/andw/xorw/addw/subw/cmpw/testw with
   immediate and register), plus signed and unsigned compares with jumps and adc/sbb chains.
   Output: checksums (against the native build of the same program) */
#include <stdio.h>

typedef struct { unsigned short flags; short val; unsigned short msk; short d; } File;

static volatile int g_zero;              /* keeps the compiler from computing everything in advance */

static unsigned pass(File *f, int n)
{
    unsigned h = 0;
    int i;
    for (i = 0; i < n; i++) {
        f->flags |= 0x0004;
        if (i & 1) f->flags &= (unsigned short)~0x0010; else f->flags |= 0x0010;
        f->flags ^= (unsigned short)(i * 0x0101);
        f->val += (short)(i * 7 - 300);
        f->val -= (short)(i & 0xff);
        if (f->val < -1000) h += 1;                                   /* cmpw signed */
        if ((unsigned short)f->val > 40000) h += 2;                   /* unsigned */
        if (f->flags & 0x8000) h += 4;                                 /* testw */
        if ((f->flags & 0x00ff) == 0x0014) h += 8;
        if (f->msk == f->flags) h += 16;
        f->msk = (unsigned short)(f->msk + f->flags + (f->val < 0));
        if ((short)f->msk >= f->d) h += 32;
        f->d = (short)(f->d - 0x1234 + g_zero);
        h = h * 31 + f->flags + (unsigned short)f->val + f->msk + (unsigned short)f->d;
    }
    return h;
}

/* register forms: counters and masks as short in registers */
static unsigned reg16(int n)
{
    unsigned short a = 0x1234, b = 0xfedc;
    short c = -5;
    unsigned h = 0;
    int i;
    for (i = 0; i < n; i++) {
        a = (unsigned short)(a + b);
        b = (unsigned short)(b ^ (a | 0x0101));
        c = (short)(c - (short)(a & 0x7f));
        if (c < 0) c = (short)(c + 1000);
        if (a > b) h += 3;
        if ((short)a < (short)b) h += 5;
        h = h * 33 + a + b + (unsigned short)c;
    }
    return h;
}

/* shifting and counting with 16 bits in the register, movw $imm to memory, rol/ror with 32 bits */
static unsigned short g_field[64];

static unsigned shift16(int n)
{
    unsigned short a = 0xbeef, z = 300;
    short s = -12345;
    unsigned x = 0x12345678u, h = 0;
    int i;
    for (i = 0; i < n; i++) {
        a = (unsigned short)(a >> (1 + (i & 3)) | 0x8000);         /* shrw */
        s = (short)(s >> 2);                                        /* sarw */
        if (s == 0 || s == -1) s = (short)(-12345 + i);
        a = (unsigned short)(a << 1);                               /* shlw / addw */
        z--;                                                        /* decw */
        if (z == 0) z = 300;
        g_field[i & 63] = 0x7f01;                                    /* movw $imm */
        g_field[(i * 7) & 63] = (unsigned short)(0xa55a ^ i);
        x = (x >> 5) | (x << 27);                                   /* rorl */
        x = (x << 3) | (x >> 29);                                   /* roll */
        x += a;
        h = h * 37 + a + (unsigned short)s + z + g_field[(i * 3) & 63] + (x & 0xffff);
    }
    return h ^ x;
}

int main(void)
{
    File f = { 0x1200, 100, 0xffff, 0x7000 };
    unsigned long long x = 0xffff0000fff0ull, y = 0x0000ffff00ffull;
    int i;
    printf("memory %08x\n", pass(&f, 5000 + g_zero));
    printf("register %08x\n", reg16(7000 + g_zero));
    printf("shift %08x\n", shift16(9000 + g_zero));
    for (i = 0; i < 1000; i++) { x += y; y -= x >> 3; }               /* adc/sbb chains (32 bit) */
    printf("chains %08x%08x %08x%08x\n", (unsigned)(x >> 32), (unsigned)x, (unsigned)(y >> 32), (unsigned)y);
    printf("end %04x %d %04x %d\n", f.flags, f.val, f.msk, f.d);
    return 0;
}
