/* bench_libm.c - floating point with libm and double return via x87 (st0): sin, cos, exp, log, pow, atan2, fmod,
   floor, sqrt. Comparison: translator against interpreter (both compute with the same Intel libm; the PowerPC libm
   rounds differently in the last bits). Usage: bench_libm [rounds] */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

double __attribute__((noinline)) wave(double x, double y)
{
    return sin(x) * cos(y) + exp(-x * x) - log(1.0 + y * y);
}

double __attribute__((noinline)) mixture(double x)
{
    return pow(x, 1.7) + atan2(x, 1.0 + x) + fmod(x * 13.0, 7.0) + floor(x * 3.5);
}

float __attribute__((noinline)) simple(float x)
{
    return sqrtf(x) * 0.5f + x;
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 1, r, i;
    double h = 0;
    float g = 0;
    for (r = 0; r < rounds; r++)
        for (i = 0; i < 2000; i++) {
            double x = i * 0.0031 + r * 0.01;
            h += wave(x, x * 0.7);
            h += mixture(x + 0.1);
            g += simple((float)x);
        }
    printf("result %.17g %.9g\n", h, g);
    return 0;
}
