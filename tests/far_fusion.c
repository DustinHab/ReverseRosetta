/* far_fusion.c - a compare whose only reader (jcc, setcc, cmov) comes a few instructions later, with loads, stores and
 * register moves in between that leave the flags alone (gcc's scheduling: cmpl $0x21,%eax; movw %di,(%ecx,%eax,2);
 * jg): the translator merges them all the same and keeps the condition in cr2. Every condition code, sub/and/test as
 * the setter, x87 code (C calls) in between. Translated (ROSETTA_THRESHOLD=1, also ROSETTA_NO_FAR=1) against the
 * interpreter (ROSETTA_INTERP=1). */
#include <stdio.h>
#include <math.h>

static int mem[64];

/* jcc after two instructions: counts how often condition CC holds for the pairs (a[i], b[i]) */
#define JCC(name, cc)                                                                     \
static int name(const int *a, const int *b, int n)                                        \
{                                                                                         \
    int i, c = 0;                                                                         \
    for (i = 0; i < n; i++) {                                                             \
        int x = a[i], y = b[i], t;                                                        \
        __asm__ volatile("cmpl %3, %2\n\t"                                                \
                         "movl %2, %1\n\t"                                                \
                         "movl %1, %2\n\t"                                                \
                         "j" #cc " 1f\n\t"                                                \
                         "incl %0\n"                                                      \
                         "1:"                                                             \
                         : "+r"(c), "=m"(mem[i & 31]), "=&r"(t) : "r"(y), "2"(x) : "cc"); \
    }                                                                                     \
    return c;                                                                             \
}
JCC(j_o, o) JCC(j_no, no) JCC(j_b, b) JCC(j_ae, ae) JCC(j_e, e) JCC(j_ne, ne) JCC(j_be, be) JCC(j_a, a)
JCC(j_s, s) JCC(j_ns, ns) JCC(j_p, p) JCC(j_np, np) JCC(j_l, l) JCC(j_ge, ge) JCC(j_le, le) JCC(j_g, g)

/* setcc and cmov after loads in between; sub, and and test as the setter */
static unsigned int set_cmov(const int *a, const int *b, int n)
{
    unsigned int h = 0;
    int i;
    for (i = 0; i < n; i++) {
        int x = a[i], y = b[i], r1 = 0, r2 = 7, r3 = 0;
        unsigned char s1 = 0, s2 = 0;
        __asm__ volatile("subl %5, %4\n\t"
                         "movl 4(%6), %2\n\t"
                         "setl %0\n\t"
                         : "=q"(s1), "=m"(mem[0]), "=&r"(r1), "=m"(mem[1]), "+r"(x) : "r"(y), "r"(mem) : "cc");
        __asm__ volatile("andl %4, %3\n\t"
                         "movl %3, 8(%5)\n\t"
                         "movl 12(%5), %1\n\t"
                         "cmovel %4, %1\n\t"
                         "testl %4, %4\n\t"
                         "leal 3(%4), %2\n\t"
                         "seta %0\n\t"
                         : "=q"(s2), "+r"(r2), "=&r"(r3), "+r"(x) : "r"(y), "r"(mem) : "cc", "memory");
        h = h * 31 + (unsigned int)s1 * 3 + s2 + (unsigned int)r1 + (unsigned int)r2 * 5 + (unsigned int)r3;
    }
    return h;
}

/* x87 in between (sine through the system's libm: C code that must keep cr2) */
static int with_x87(const int *a, const int *b, int n)
{
    int i, c = 0;
    for (i = 0; i < n; i++) {
        double d = a[i] * 1e-6;
        __asm__ volatile("cmpl %3, %2\n\t"
                         "fldl %1\n\t"
                         "fsin\n\t"
                         "fstpl %1\n\t"
                         "jle 1f\n\t"
                         "incl %0\n"
                         "1:"
                         : "+r"(c), "+m"(d) : "r"(a[i]), "r"(b[i]) : "cc");
        c += d > 0.5;
    }
    return c;
}

int main(void)
{
    static int a[4000], b[4000];
    int (*f[16])(const int *, const int *, int) = { j_o, j_no, j_b, j_ae, j_e, j_ne, j_be, j_a,
                                                   j_s, j_ns, j_p, j_np, j_l, j_ge, j_le, j_g };
    unsigned int s = 1;
    int i;
    for (i = 0; i < 4000; i++) {
        s = s * 1103515245u + 12345u;
        a[i] = (int)(s >> 3) - (i % 7 == 0 ? 0x40000000 : 0);
        s = s * 1103515245u + 12345u;
        b[i] = i % 5 == 0 ? a[i] : (int)(s >> 2) - 0x20000000;
        if (i % 11 == 0) b[i] = (int)0x80000000u;
    }
    for (i = 0; i < 16; i++) printf("%d ", f[i](a, b, 4000));
    printf("\nsetcc/cmov %08x, x87 %d\n", set_cmov(a, b, 4000), with_x87(a, b, 4000));
    return 0;
}
