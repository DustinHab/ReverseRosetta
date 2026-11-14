/* fpcompare.c - floating-point compares (ucomisd/ucomiss + jump, setcc, cmov) with NaN, +-0, infinity; plus printf
   of float/double. The output depends only on the C semantics: the PowerPC version is the reference. */
#include <stdio.h>
#include <math.h>

static volatile double dw[] = { 0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 1e300, -1e300, 1e-300, 3.25, 0, 0 };
static volatile float fw[] = { 0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 2.0f, 1e30f, -1e30f, 1e-30f, 3.25f, 0, 0 };
#define N 12

static unsigned h;
static void m(unsigned v) { h = h * 31 + v; }

static __attribute__((noinline)) int jump_d(double a, double b)
{
    int r = 0;
    if (a == b) r |= 1;
    if (a != b) r |= 2;
    if (a < b) r |= 4;
    if (a <= b) r |= 8;
    if (a > b) r |= 16;
    if (a >= b) r |= 32;
    if (!(a < b)) r |= 64;
    if (!(a >= b)) r |= 128;
    return r;
}

static __attribute__((noinline)) int jump_f(float a, float b)
{
    int r = 0;
    if (a == b) r |= 1;
    if (a != b) r |= 2;
    if (a < b) r |= 4;
    if (a <= b) r |= 8;
    if (a > b) r |= 16;
    if (a >= b) r |= 32;
    return r;
}

static __attribute__((noinline)) int vals_d(double a, double b)
{
    return (a == b) + 2 * (a != b) + 4 * (a < b) + 8 * (a <= b) + 16 * (a > b) + 32 * (a >= b);
}

static __attribute__((noinline)) double larger(double a, double b) { return a > b ? a : b; }
static __attribute__((noinline)) float less(float a, float b) { return a < b ? a : b; }

int main(void)
{
    int i, j;
    char text[64];
    dw[10] = NAN; dw[11] = INFINITY;
    fw[10] = NAN; fw[11] = -INFINITY;
    for (i = 0; i < N; i++)
        for (j = 0; j < N; j++) {
            m((unsigned)jump_d(dw[i], dw[j]));
            m((unsigned)jump_f(fw[i], fw[j]));
            m((unsigned)vals_d(dw[i], dw[j]));
            m(larger(dw[i], dw[j]) == dw[i]);
            m(less(fw[i], fw[j]) == fw[i]);
        }
    printf("%08x\n", h);
    for (i = 0; i < N; i++) {
        snprintf(text, sizeof text, "%.9g %.17g %g %e", (double)fw[i], dw[i], dw[i] * 3.0, (double)fw[i] * 7.0f);
        printf("%s\n", text);
    }
    printf("%.9g %.9g %.9g\n", 15736.4775, (double)15736.4775f, 10000.0);
    return 0;
}
