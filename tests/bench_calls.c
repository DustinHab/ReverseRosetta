/* bench_calls.c - cost of calls: direct, via function pointer (one target), via pointer with changing targets,
   recursion. Usage: bench_calls [rounds] */
#include <stdio.h>
#include <stdlib.h>

static int __attribute__((noinline)) one(int x) { return x + 1; }
static int __attribute__((noinline)) two(int x) { return x ^ 0x55; }
static int __attribute__((noinline)) three(int x) { return x * 3; }
static int __attribute__((noinline)) four(int x) { return x - 7; }
static int (*const ftab[4])(int) = { one, two, three, four };
static int __attribute__((noinline)) deep(int n) { return n == 0 ? 0 : deep(n - 1) + 1; }

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 1, r, i;
    volatile int (*ptr)(int) = (volatile int (*)(int))one;
    unsigned s = 1;
    for (r = 0; r < rounds; r++) {
        for (i = 0; i < 200000; i++) s = (unsigned)one((int)s);
        for (i = 0; i < 200000; i++) s = (unsigned)((int (*)(int))ptr)((int)s);
        for (i = 0; i < 200000; i++) s = (unsigned)ftab[i & 3]((int)s);
        for (i = 0; i < 2000; i++) s += (unsigned)deep(100);
    }
    printf("result %08x\n", s);
    return 0;
}
