/* bench_integer.c - integer benchmark load for the translator. The interpreter counts the instructions
   (ROSETTA_INTERP=1 ROSETTA_STATS=1), the time comes from the translator: MIPS = instructions / time.
   Usage: bench_integer [rounds] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned sieve(int n, char *s)
{
    unsigned count = 0;
    int i, j;
    memset(s, 0, (size_t)n);
    for (i = 2; i < n; i++)
        if (!s[i]) {
            count++;
            for (j = 2 * i; j < n; j += i) s[j] = 1;
        }
    return count;
}

static unsigned fnv(const unsigned char *p, int n)
{
    unsigned h = 2166136261u;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

static void sort(int *a, int n)
{
    int i, j, x;
    for (i = 1; i < n; i++) {
        x = a[i];
        for (j = i - 1; j >= 0 && a[j] > x; j--) a[j + 1] = a[j];
        a[j + 1] = x;
    }
}

static unsigned collatz(unsigned bis)
{
    unsigned n, longest = 0, s;
    for (n = 1; n < bis; n++) {
        unsigned x = n;
        s = 0;
        while (x != 1) { x = (x & 1) ? 3 * x + 1 : x >> 1; s++; }
        if (s > longest) longest = s;
    }
    return longest;
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 1, r, i;
    static char s[200000];
    static unsigned char pbuf[100000];
    static int field[2000];
    unsigned h = 0, x = 12345;
    for (r = 0; r < rounds; r++) {
        h += sieve(200000, s);
        for (i = 0; i < (int)sizeof pbuf; i++) pbuf[i] = (unsigned char)(i * 7 + r);
        h ^= fnv(pbuf, sizeof pbuf);
        for (i = 0; i < 2000; i++) { x = x * 1103515245u + 12345u; field[i] = (int)(x >> 8); }
        sort(field, 2000);
        h += (unsigned)field[1000];
        h += collatz(20000);
    }
    printf("result %08x\n", h);
    return 0;
}
