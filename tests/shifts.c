/* shifts.c - 64-bit shifts (gcc: shld/shrd) and shld/shrd themselves with their flags (CF, ZF, SF) by a fixed count
 * and by cl; byte shifts and rotations (low and high byte registers, with flags), rolw/rorw, rol/ror by cl, xchg
 * eax, movsbw/movzbw. Hot loops, so they run translated. The output must match the native PowerPC build, which
 * computes the same in C. */
#include <stdio.h>
#include <stdint.h>

static uint64_t mix(uint64_t x) { return x * 6364136223846793005ull + 1442695040888963407ull; }

#ifdef __i386__
static uint32_t shld_cl(uint32_t d, uint32_t s, int n, int *f)
{
    unsigned char c, z, g;
    __asm__ volatile("shldl %%cl, %4, %0; setc %1; setz %2; sets %3" : "+r"(d), "=m"(c), "=m"(z), "=m"(g) : "r"(s), "c"(n) : "cc");
    *f = c | z << 1 | g << 2;
    return d;
}
static uint32_t shrd_cl(uint32_t d, uint32_t s, int n, int *f)
{
    unsigned char c, z, g;
    __asm__ volatile("shrdl %%cl, %4, %0; setc %1; setz %2; sets %3" : "+r"(d), "=m"(c), "=m"(z), "=m"(g) : "r"(s), "c"(n) : "cc");
    *f = c | z << 1 | g << 2;
    return d;
}
static uint32_t shld_7(uint32_t d, uint32_t s, int *f)
{
    unsigned char c, z, g;
    __asm__ volatile("shldl $7, %4, %0; setc %1; setz %2; sets %3" : "+r"(d), "=m"(c), "=m"(z), "=m"(g) : "r"(s) : "cc");
    *f = c | z << 1 | g << 2;
    return d;
}
static uint32_t shrd_7(uint32_t d, uint32_t s, int *f)
{
    unsigned char c, z, g;
    __asm__ volatile("shrdl $7, %4, %0; setc %1; setz %2; sets %3" : "+r"(d), "=m"(c), "=m"(z), "=m"(g) : "r"(s) : "cc");
    *f = c | z << 1 | g << 2;
    return d;
}
static uint32_t shld_mem(uint32_t d, uint32_t s)    /* memory destination, flags not needed */
{
    volatile uint32_t m = d;
    __asm__ volatile("shldl $13, %1, %0" : "+m"(m) : "r"(s) : "cc");
    return m;
}
static uint32_t shrd_mem_cl(uint32_t d, uint32_t s, int n)
{
    volatile uint32_t m = d;
    __asm__ volatile("shrdl %%cl, %1, %0" : "+m"(m) : "r"(s), "c"(n) : "cc");
    return m;
}
/* byte and 16-bit forms, rotations by cl, xchg, movsbw/movzbw */
static uint32_t shrb3_al(uint32_t x, int *f)
{
    unsigned char c, z, g;
    __asm__ volatile("shrb $3, %%al; setc %1; setz %2; sets %3" : "+a"(x), "=m"(c), "=m"(z), "=m"(g) : : "cc");
    *f = c | z << 1 | g << 2;
    return x;
}
static uint32_t shlb2_ah(uint32_t x, int *f)
{
    unsigned char c, z, g;
    __asm__ volatile("shlb $2, %%ah; setc %1; setz %2; sets %3" : "+a"(x), "=m"(c), "=m"(z), "=m"(g) : : "cc");
    *f = c | z << 1 | g << 2;
    return x;
}
static uint32_t sarb1_dl(uint32_t x, int *f)
{
    unsigned char c, z, g;
    __asm__ volatile("sarb $1, %%dl; setc %1; setz %2; sets %3" : "+d"(x), "=m"(c), "=m"(z), "=m"(g) : : "cc");
    *f = c | z << 1 | g << 2;
    return x;
}
static uint32_t rolb5_cl(uint32_t x) { __asm__ volatile("rolb $5, %%cl" : "+c"(x) : : "cc"); return x; }
static uint32_t rorb3_ch(uint32_t x) { __asm__ volatile("rorb $3, %%ch" : "+c"(x) : : "cc"); return x; }
static uint32_t rolw8_dx(uint32_t x) { __asm__ volatile("rolw $8, %%dx" : "+d"(x) : : "cc"); return x; }
static uint32_t rorw4_ax(uint32_t x) { __asm__ volatile("rorw $4, %%ax" : "+a"(x) : : "cc"); return x; }
static uint32_t roll_cl(uint32_t x, int n) { __asm__ volatile("roll %%cl, %0" : "+r"(x) : "c"(n) : "cc"); return x; }
static uint32_t rorl_cl(uint32_t x, int n) { __asm__ volatile("rorl %%cl, %0" : "+r"(x) : "c"(n) : "cc"); return x; }
static uint32_t xchg_ad(uint32_t a, uint32_t d) { __asm__ volatile("xchgl %%eax, %%edx" : "+a"(a), "+d"(d)); return a * 3 + d; }
static uint32_t movsbw_(uint32_t x, uint32_t s) { __asm__ volatile("movsbw %b1, %w0" : "+r"(x) : "q"(s)); return x; }
static uint32_t movzbw_(uint32_t x, uint32_t s) { __asm__ volatile("movzbw %b1, %w0" : "+r"(x) : "q"(s)); return x; }
#else
static int fb(uint32_t r, int cf) { return cf | ((r & 0xff) == 0) << 1 | (int)((r >> 7) & 1) << 2; }
static uint32_t shrb3_al(uint32_t x, int *f) { uint32_t b = x & 0xff, r = b >> 3; *f = fb(r, (b >> 2) & 1); return (x & ~0xffu) | r; }
static uint32_t shlb2_ah(uint32_t x, int *f) { uint32_t b = (x >> 8) & 0xff, r = (b << 2) & 0xff; *f = fb(r, (b >> 6) & 1); return (x & ~0xff00u) | r << 8; }
static uint32_t sarb1_dl(uint32_t x, int *f) { int b = (signed char)(x & 0xff); uint32_t r = (uint32_t)(b >> 1) & 0xff; *f = fb(r, b & 1); return (x & ~0xffu) | r; }
static uint32_t rolb5_cl(uint32_t x) { uint32_t b = x & 0xff; return (x & ~0xffu) | (((b << 5) | (b >> 3)) & 0xff); }
static uint32_t rorb3_ch(uint32_t x) { uint32_t b = (x >> 8) & 0xff; return (x & ~0xff00u) | (((b >> 3) | (b << 5)) & 0xff) << 8; }
static uint32_t rolw8_dx(uint32_t x) { uint32_t h = x & 0xffff; return (x & ~0xffffu) | (((h << 8) | (h >> 8)) & 0xffff); }
static uint32_t rorw4_ax(uint32_t x) { uint32_t h = x & 0xffff; return (x & ~0xffffu) | (((h >> 4) | (h << 12)) & 0xffff); }
static uint32_t roll_cl(uint32_t x, int n) { n &= 31; return n ? x << n | x >> (32 - n) : x; }
static uint32_t rorl_cl(uint32_t x, int n) { n &= 31; return n ? x >> n | x << (32 - n) : x; }
static uint32_t xchg_ad(uint32_t a, uint32_t d) { return d * 3 + a; }
static uint32_t movsbw_(uint32_t x, uint32_t s) { return (x & ~0xffffu) | ((uint32_t)(short)(signed char)(s & 0xff) & 0xffff); }
static uint32_t movzbw_(uint32_t x, uint32_t s) { return (x & ~0xffffu) | (s & 0xff); }
static uint32_t ld(uint32_t d, uint32_t s, int n) { n &= 31; return n ? d << n | s >> (32 - n) : d; }
static uint32_t rd(uint32_t d, uint32_t s, int n) { n &= 31; return n ? d >> n | s << (32 - n) : d; }
static int fl(uint32_t r, int cf) { return cf | (r == 0) << 1 | (int)(r >> 31) << 2; }
static uint32_t shld_cl(uint32_t d, uint32_t s, int n, int *f) { uint32_t r = ld(d, s, n); *f = fl(r, (d >> (32 - (n & 31))) & 1); return r; }
static uint32_t shrd_cl(uint32_t d, uint32_t s, int n, int *f) { uint32_t r = rd(d, s, n); *f = fl(r, (d >> ((n & 31) - 1)) & 1); return r; }
static uint32_t shld_7(uint32_t d, uint32_t s, int *f) { return shld_cl(d, s, 7, f); }
static uint32_t shrd_7(uint32_t d, uint32_t s, int *f) { return shrd_cl(d, s, 7, f); }
static uint32_t shld_mem(uint32_t d, uint32_t s) { return ld(d, s, 13); }
static uint32_t shrd_mem_cl(uint32_t d, uint32_t s, int n) { return rd(d, s, n); }
#endif

int main(void)
{
    uint64_t x = 1, sum = 0, fsum = 0;
    int i, k, f;
    for (i = 0; i < 4000; i++) {            /* 64-bit shifts as gcc compiles them */
        x = mix(x);
        k = (int)(x >> 58);
        sum = sum * 31 + (x << k) + (x >> k) + (uint64_t)((int64_t)x >> k) + (x << 8) + (x >> 24) + (uint64_t)((int64_t)x >> 40);
    }
    printf("64-bit shifts: %08x%08x\n", (unsigned)(sum >> 32), (unsigned)sum);
    sum = 0;
    for (i = 0; i < 4000; i++) {            /* shld/shrd directly, counts 1..31 (count 0 leaves the flags: not compared) */
        uint32_t d, s, r;
        x = mix(x);
        d = (uint32_t)x; s = (uint32_t)(x >> 32);
        k = 1 + (int)((x >> 13) % 31);
        if ((i & 63) == 0) d = 0, s &= 0xffff;   /* now and then a zero result */
        r = shld_cl(d, s, k, &f); sum = sum * 33 + r; fsum = fsum * 7 + (uint64_t)f;
        r = shrd_cl(d, s, k, &f); sum = sum * 33 + r; fsum = fsum * 7 + (uint64_t)f;
        r = shld_7(d, s, &f); sum = sum * 33 + r; fsum = fsum * 7 + (uint64_t)f;
        r = shrd_7(d, s, &f); sum = sum * 33 + r; fsum = fsum * 7 + (uint64_t)f;
        sum = sum * 33 + shld_mem(d, s) + shrd_mem_cl(d, s, k) + shrd_mem_cl(d, s, 0);
    }
    printf("shld/shrd: values %08x%08x, flags %08x%08x\n", (unsigned)(sum >> 32), (unsigned)sum, (unsigned)(fsum >> 32), (unsigned)fsum);
    sum = fsum = 0;
    for (i = 0; i < 4000; i++) {            /* byte and 16-bit forms, rotations by cl, xchg, movsbw/movzbw */
        uint32_t a, d;
        x = mix(x);
        a = (uint32_t)x; d = (uint32_t)(x >> 32);
        if ((i & 31) == 0) a &= 0xffff0000u;     /* zero bytes now and then */
        k = (int)((x >> 7) & 63);
        sum = sum * 33 + shrb3_al(a, &f); fsum = fsum * 7 + (uint64_t)f;
        sum = sum * 33 + shlb2_ah(a, &f); fsum = fsum * 7 + (uint64_t)f;
        sum = sum * 33 + sarb1_dl(d, &f); fsum = fsum * 7 + (uint64_t)f;
        sum = sum * 33 + rolb5_cl(a) + rorb3_ch(d) + rolw8_dx(a) + rorw4_ax(d);
        sum = sum * 33 + roll_cl(a, k) + rorl_cl(d, k) + xchg_ad(a, d) + movsbw_(a, d) + movzbw_(d, a);
    }
    printf("byte, word, cl rotations, xchg, movsbw: values %08x%08x, flags %08x%08x\n", (unsigned)(sum >> 32), (unsigned)sum,
           (unsigned)(fsum >> 32), (unsigned)fsum);
    return 0;
}
