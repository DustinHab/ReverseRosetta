/* bench_calls_part.c - parts of bench_calls one at a time (-DPART=1 direct, 2 pointer, 3 table, 4 recursion): direct, via
   function pointer (one target), via pointer with changing targets, recursion. Usage: bench_calls_part [rounds] */
#include <stdio.h>
#include <stdlib.h>

#ifndef PART
#define PART 1
#endif

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
#if PART == 1
        for (i = 0; i < 200000; i++) s = (unsigned)one((int)s);
#elif PART == 2
        for (i = 0; i < 200000; i++) s = (unsigned)((int (*)(int))ptr)((int)s);
#elif PART == 3
        for (i = 0; i < 200000; i++) s = (unsigned)ftab[i & 3]((int)s);
#else
        for (i = 0; i < 2000; i++) s += (unsigned)deep(100);
#endif
    }
    printf("result %08x\n", s);
    return 0;
}
