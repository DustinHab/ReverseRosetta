/* array_end.c - double arrays up to exactly the end of a memory page, nothing mapped behind it: hoisting the array
   accesses reads unmapped memory there and must survive that. Also strides via the index and backwards. */
#include <stdio.h>
#include <sys/mman.h>

static double __attribute__((noinline)) sum(const double *d, int n)
{
    double h = 0;
    int i;
    for (i = 0; i < n; i++) h += d[i] * d[i];
    return h;
}

static double __attribute__((noinline)) backward(const double *d, int n)
{
    double h = 0;
    int i;
    for (i = n - 1; i >= 0; i--) h += d[i] / 3.0;
    return h;
}

static double __attribute__((noinline)) col(const double *d, int n, int step)
{
    double h = 0;
    int i;
    for (i = 0; i < n; i += step) h = h * 0.5 + d[i];
    return h;
}

int main(void)
{
    char *s = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    double *d, h = 0;
    int i, r;
    if (s == MAP_FAILED) return 1;
    munmap(s + 4096, 4096);
    d = (double *)(s + 4096) - 512;             /* fills the first page exactly */
    for (i = 0; i < 512; i++) d[i] = i * 0.5 + 1;
    for (r = 0; r < 3; r++) h += sum(d, 512);
    h += backward(d, 512);
    h += col(d, 512, 7) + col(d + 3, 509, 16);
    printf("result %.17g\n", h);
    return 0;
}
