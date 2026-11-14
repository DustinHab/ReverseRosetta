/* raw_arith.c - integer arithmetic test without libSystem. Prints checksums in hex; the expected values are in
   raw_arith.expected (from the same source on real Intel hardware). */
typedef unsigned int u32;
typedef unsigned long long u64;

#ifdef REF
/* expected values on the PC: zig cc -target x86-windows -O1 -DREF raw_arith.c */
#include <stdio.h>
static int sys3(int n, int a, int b, int c) { (void)a; if (n == 4) fwrite((const void *)b, 1, (size_t)c, stdout); return 0; }
#define start test_start                /* "start" already exists in the Windows C runtime */
void start(void);
int main(void) { setvbuf(stdout, 0, _IONBF, 0); start(); return 0; }
#define LIST_END return
#else
static int sys3(int n, int a, int b, int c)
{
    int r;
    __asm__ volatile("pushl %4; pushl %3; pushl %2; pushl $0; int $0x80; addl $16,%%esp"
                     : "=a"(r) : "a"(n), "g"(a), "g"(b), "g"(c) : "memory");
    return r;
}
#define LIST_END for (;;)
#endif

static void line(const char *name, u32 v)
{
    char t[64];
    int i = 0, k;
    while (name[i]) { t[i] = name[i]; i++; }
    t[i++] = ' ';
    for (k = 7; k >= 0; k--) t[i++] = "0123456789abcdef"[(v >> (k * 4)) & 15];
    t[i++] = '\n';
    sys3(4, 1, (int)t, i);
}

static u32 shuffle(u32 h, u32 v) { return (h ^ v) * 16777619u; }

/* read the flags after an operation (pushf); only those defined by Intel (AF is undefined after logic
   instructions) */
#define FLAGS(op, a, b, m) ({ u32 f_, r_ = (a); __asm__ volatile(op " %2,%0; pushfl; popl %1" : "+r"(r_), "=r"(f_) : "r"(b) : "cc"); h = shuffle(shuffle(h, r_), f_ & (m)); })
#define ARI 0x8d5
#define LOG 0x8c5

void start(void)
{
    static const u32 vals[] = { 0, 1, 2, 7, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000,
                                 0xffffffff, 0x12345678, 0xdeadbeef, 0x55555555 };
    u32 h, i, j, a, b;
    u64 p;
    int n = sizeof vals / sizeof vals[0];

    h = 2166136261u;
    for (i = 0; i < (u32)n; i++)
        for (j = 0; j < (u32)n; j++) {
            a = vals[i]; b = vals[j];
            FLAGS("addl", a, b, ARI); FLAGS("subl", a, b, ARI); FLAGS("andl", a, b, LOG); FLAGS("orl", a, b, LOG);
            FLAGS("xorl", a, b, LOG); FLAGS("cmpl", a, b, ARI); FLAGS("testl", a, b, LOG);
            /* set the carry beforehand: otherwise it comes from the code before (after a ret) */
            FLAGS("cmpl %2,%0; adcl", a, b, ARI); FLAGS("cmpl %2,%0; sbbl", a, b, ARI);
        }
    line("alu32", h);

    h = 2166136261u;
    for (i = 0; i < (u32)n; i++)
        for (j = 0; j < 32; j++) {
            u32 r, f;
            a = vals[i];
            __asm__ volatile("movl %2,%%ecx; shll %%cl,%0; pushfl; popl %1" : "=r"(r), "=r"(f) : "r"(j), "0"(a) : "ecx", "cc");
            h = shuffle(shuffle(h, r), j ? f & (j == 1 ? 0x8c5 : 0xc5) : 0);
            __asm__ volatile("movl %2,%%ecx; shrl %%cl,%0; pushfl; popl %1" : "=r"(r), "=r"(f) : "r"(j), "0"(a) : "ecx", "cc");
            h = shuffle(shuffle(h, r), j ? f & 0xc5 : 0);
            __asm__ volatile("movl %2,%%ecx; sarl %%cl,%0; pushfl; popl %1" : "=r"(r), "=r"(f) : "r"(j), "0"(a) : "ecx", "cc");
            h = shuffle(shuffle(h, r), j ? f & 0xc5 : 0);
            __asm__ volatile("movl %2,%%ecx; roll %%cl,%0" : "=r"(r) : "r"(j), "0"(a) : "ecx", "cc");
            h = shuffle(h, r);
            __asm__ volatile("movl %2,%%ecx; rorl %%cl,%0" : "=r"(r) : "r"(j), "0"(a) : "ecx", "cc");
            h = shuffle(h, r);
        }
    line("shift", h);

    h = 2166136261u;
    for (i = 0; i < (u32)n; i++)
        for (j = 0; j < (u32)n; j++) {
            a = vals[i]; b = vals[j];
            p = (u64)a * b;
            h = shuffle(shuffle(h, (u32)p), (u32)(p >> 32));
            p = (u64)((long long)(int)a * (int)b);
            h = shuffle(shuffle(h, (u32)p), (u32)(p >> 32));
            if (b) { h = shuffle(h, a / b); h = shuffle(h, a % b); }
            if (b && !((int)a == (int)0x80000000 && (int)b == -1)) { h = shuffle(h, (u32)((int)a / (int)b)); h = shuffle(h, (u32)((int)a % (int)b)); }
        }
    line("muldiv", h);

    h = 2166136261u;
    for (i = 0; i < (u32)n; i++) {
        u32 r;
        a = vals[i];
        __asm__ volatile("bswap %0" : "=r"(r) : "0"(a)); h = shuffle(h, r);
        if (a) { __asm__ volatile("bsfl %1,%0" : "=r"(r) : "r"(a)); h = shuffle(h, r); __asm__ volatile("bsrl %1,%0" : "=r"(r) : "r"(a)); h = shuffle(h, r); }
        h = shuffle(h, (u32)(signed char)a);
        h = shuffle(h, (u32)(short)a);
        h = shuffle(h, (u32)(unsigned char)a);
        h = shuffle(h, (u32)(unsigned short)a);
        for (j = 0; j < 32; j += 5) {
            u32 f;
            __asm__ volatile("btl %2,%1; pushfl; popl %0" : "=r"(f) : "r"(a), "r"(j) : "cc"); h = shuffle(h, f & 1);
        }
    }
    line("bits", h);

    /* loop with jump conditions: sum of all signed compares */
    h = 0;
    for (i = 0; i < (u32)n; i++)
        for (j = 0; j < (u32)n; j++) {
            int x = (int)vals[i], y = (int)vals[j];
            h = h * 3 + (x < y) + 2 * (x <= y) + 4 * (vals[i] < vals[j]) + 8 * (vals[i] >= vals[j]);
        }
    line("compare", h);

    sys3(1, 0, 0, 0);
    LIST_END ;
}
