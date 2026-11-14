/* bench_float.c - floating-point benchmark load: matrix product, n-body with sqrt, float vectors, conversions, calls with
   double return (x87 st0). Only IEEE-exact operations (no sin/exp), so native and translated code compute
   bit-identical results; the PowerPC version is built without fused multiply-add. Usage: bench_float [rounds] */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define N 40

static double a[N][N], b[N][N], c[N][N];
static float fx[4096], fy[4096];

static __attribute__((noinline)) double len(double x, double y, double z) { return sqrt(x * x + y * y + z * z); }

static __attribute__((noinline)) float point(const float *p, const float *q, int n)
{
    float s = 0.0f;
    int i;
    for (i = 0; i < n; i++) s += p[i] * q[i];
    return s;
}

static double body(int steps)
{
    double px[5] = { 0, 1, -1, 0.5, -0.3 }, py[5] = { 0, 0.2, 0.7, -1, 0.4 }, pz[5] = { 0, 0.1, -0.2, 0.3, -0.5 };
    double vx[5] = { 0, 0.01, -0.02, 0.03, 0 }, vy[5] = { 0.02, 0, 0.01, -0.01, 0.03 }, vz[5] = { 0, 0, 0, 0.01, -0.01 };
    double m[5] = { 10, 1, 0.5, 0.8, 0.3 }, e = 0;
    int s, i, j;
    for (s = 0; s < steps; s++) {
        for (i = 0; i < 5; i++)
            for (j = i + 1; j < 5; j++) {
                double dx = px[i] - px[j], dy = py[i] - py[j], dz = pz[i] - pz[j];
                double d = len(dx, dy, dz) + 0.01, f = 0.001 / (d * d * d);
                vx[i] -= dx * m[j] * f; vy[i] -= dy * m[j] * f; vz[i] -= dz * m[j] * f;
                vx[j] += dx * m[i] * f; vy[j] += dy * m[i] * f; vz[j] += dz * m[i] * f;
            }
        for (i = 0; i < 5; i++) { px[i] += vx[i]; py[i] += vy[i]; pz[i] += vz[i]; }
    }
    for (i = 0; i < 5; i++) e += m[i] * (vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i]);
    return e;
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 1, r, i, j, k;
    double h = 0;
    long integer = 0;
    for (i = 0; i < N; i++)
        for (j = 0; j < N; j++) { a[i][j] = (i * 7 + j * 3) % 11 * 0.125 - 0.5; b[i][j] = (i + 2 * j) % 13 * 0.0625; }
    for (i = 0; i < 4096; i++) { fx[i] = (float)(i % 37) * 0.25f; fy[i] = (float)(i % 19) * 0.125f - 1.0f; }
    for (r = 0; r < rounds; r++) {
        for (i = 0; i < N; i++)
            for (j = 0; j < N; j++) {
                double s = 0;
                for (k = 0; k < N; k++) s += a[i][k] * b[k][j];
                c[i][j] = s;
            }
        h += c[r % N][(r * 3) % N];
        h += body(200);
        for (i = 0; i < 4096; i++) fy[i] = fy[i] * 0.999f + fx[i] * 0.001f;
        h += point(fx, fy, 4096);
        for (i = 0; i < 20000; i++) { double v = i * 0.37 + r; integer += (long)(v / 3.0); h += (double)(i & 255) / (v + 1.0); }
    }
    printf("result %.17g %ld\n", h, integer);
    return 0;
}
