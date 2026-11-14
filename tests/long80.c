/* long80.c - load, store, compute and print long double (x87, 80 bit: fldt/fstpt). Results are printed rounded to
   64 bits for comparison with the real Intel behavior (the translator computes x87 in double precision) */
#include <stdio.h>
#include <math.h>

static volatile long double g_field[8];

int main(void)
{
    long double a = 1.0L, s = 0.0L;
    int i;
    for (i = 0; i < 8; i++) g_field[i] = (long double)(i + 1) / 3.0L;   /* fstpt */
    for (i = 0; i < 8; i++) s += g_field[i] * g_field[i];                 /* fldt */
    for (i = 1; i <= 20; i++) a *= (long double)i;
    printf("sum %.12f\n", (double)s);
    printf("factorial %.6e\n", (double)a);
    printf("direct %.10Lf\n", g_field[2]);
    printf("sqrt %.12f\n", (double)sqrtl(s));
    printf("compare %d %d\n", g_field[1] < g_field[2], g_field[3] == g_field[3]);
    return 0;
}
