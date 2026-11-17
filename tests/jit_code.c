/* jit_code.c - code that the program writes into an anonymous writable and executable mapping and runs, then
 * rewrites while it is in use (as JIT compilers do: SpiderMonkey, JavaScriptCore): the translator write-protects such
 * pages and must notice every change (patched constants, patched jump targets, new code in the same page, code
 * written by memcpy). The output must equal the interpreter's. */
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

typedef int (*fn)(int);
static unsigned char *g_buf;

/* mov eax,[esp+4]; imul eax,eax,K; add eax,C; ret */
static fn emit_affine(unsigned char *p, int k, int c)
{
    unsigned char code[] = { 0x8b, 0x44, 0x24, 0x04, 0x69, 0xc0, 0, 0, 0, 0, 0x05, 0, 0, 0, 0, 0xc3 };
    memcpy(code + 6, &k, 4);
    memcpy(code + 11, &c, 4);
    memcpy(p, code, sizeof code);
    return (fn)p;
}

/* a loop in generated code: sum of i*k for i < n (n at [esp+4]); k patched in place later */
static fn emit_loop(unsigned char *p, int k)
{
    p[0] = 0x8b; p[1] = 0x4c; p[2] = 0x24; p[3] = 0x04;          /* mov ecx,[esp+4] */
    p[4] = 0x31; p[5] = 0xc0;                                    /* xor eax,eax */
    p[6] = 0x31; p[7] = 0xd2;                                    /* xor edx,edx */
    p[8] = 0x39; p[9] = 0xca;                                    /* L: cmp edx,ecx */
    p[10] = 0x7d; p[11] = 0x0e;                                  /* jge end (+14) */
    p[12] = 0x50;                                                /* push eax */
    p[13] = 0x69; p[14] = 0xc2; memcpy(p + 15, &k, 4);           /* imul eax,edx,K */
    p[19] = 0x01; p[20] = 0x04; p[21] = 0x24;                    /* add [esp],eax */
    p[22] = 0x58;                                                /* pop eax */
    p[23] = 0x42;                                                /* inc edx */
    p[24] = 0xeb; p[25] = 0xee;                                  /* jmp L (-18) */
    p[26] = 0xc3;                                                /* end: ret */
    return (fn)p;
}

int main(void)
{
    int round, i, s = 0;
    fn f, g, h;
    g_buf = mmap(0, 3 * 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
    f = emit_affine(g_buf, 3, 1);
    g = emit_loop(g_buf + 64, 5);
    h = emit_affine(g_buf + 4096 - 7, 7, 2);                     /* across a page boundary */
    for (round = 0; round < 40; round++) {
        for (i = 0; i < 2000; i++) s += f(i) ^ h(i);              /* hot: translated */
        s += g(1000);
        memcpy(g_buf + 6, &round, 4);                             /* patch f's factor */
        if (round % 3 == 0) { int k = round + 2; memcpy(g_buf + 64 + 15, &k, 4); }   /* patch the loop's factor */
        if (round % 5 == 0) emit_affine(g_buf + 128, round, round);                   /* new code in the same page */
        if (round % 5 == 1) s += ((fn)(g_buf + 128))(round);
        if (round % 7 == 0) { int c = round * 11; memcpy(g_buf + 4096 - 7 + 11, &c, 4); }   /* h's constant, next page */
        g_buf[4096 * 2 + round] = (unsigned char)round;           /* data in the third page */
        printf("round %2d: %d\n", round, s);
    }
    return 0;
}
