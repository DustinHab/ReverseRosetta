/* vdsp_d.c [ROUNDS] - real SSE2 code from Apple's Intel libvDSP/libvMisc (doubles): vector arithmetic, dot product,
 * sums, extreme values, clamping, convolution, matrix product, FFT, vForce (sqrt, exp, log, sin). i386 only:
 * comparison translated against interpreter (bits of the results as checksums). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <Accelerate/Accelerate.h>

#define N 1024

static double a[N], b[N], c[N], d[N], re[N], imag[N];
static uint32_t sum;

static void mix(const char *name, const double *v, int n)
{
    uint32_t h = 2166136261u;
    const uint8_t *x = (const uint8_t *)v;
    int i;
    for (i = 0; i < 8 * n; i++) h = (h ^ x[i]) * 16777619u;
    sum = (sum ^ h) * 16777619u;
    printf("%-10s %08x  %.17g\n", name, h, v[n / 3]);
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 1, r, i, n = N;
    double s, t, lo = -2.5, hi = 3.25, k = 0.375;
    FFTSetupD fs = vDSP_create_fftsetupD(10, kFFTRadix2);
    DSPDoubleSplitComplex z;
    vDSP_Length idx;
    for (r = 0; r < rounds; r++) {
        for (i = 0; i < N; i++) { a[i] = (i % 17) * 0.25 - 2.0 + r * 1e-3; b[i] = 1.0 / (i + 1) + (i % 5); }
        vDSP_vaddD(a, 1, b, 1, c, 1, n);            if (!r) mix("vaddD", c, N);
        vDSP_vsubD(a, 1, b, 1, d, 1, n);            if (!r) mix("vsubD", d, N);
        vDSP_vmulD(c, 1, d, 1, c, 1, n);            if (!r) mix("vmulD", c, N);
        vDSP_vdivD(b, 1, a, 1, d, 1, n);            if (!r) mix("vdivD", d, N);
        vDSP_vsmaD(a, 1, &k, b, 1, d, 1, n);        if (!r) mix("vsmaD", d, N);
        vDSP_vmaD(a, 1, b, 1, c, 1, d, 1, n);       if (!r) mix("vmaD", d, N);
        vDSP_dotprD(a, 1, b, 1, &s, n);             if (!r) mix("dotprD", &s, 1);
        vDSP_svesqD(a, 1, &t, n);                   if (!r) mix("svesqD", &t, 1);
        vDSP_meanvD(b, 1, &s, n);                   if (!r) mix("meanvD", &s, 1);
        vDSP_maxviD(d, 1, &s, &idx, n);             if (!r) { mix("maxviD", &s, 1); printf("  idx %lu\n", (unsigned long)idx); }
        vDSP_minvD(d, 1, &s, n);                    if (!r) mix("minvD", &s, 1);
        vDSP_vclipD(d, 1, &lo, &hi, c, 1, n);       if (!r) mix("vclipD", c, N);
        vDSP_vabsD(a, 1, c, 1, n);                  if (!r) mix("vabsD", c, N);
        vDSP_vsqD(a, 1, c, 1, n);                   if (!r) mix("vsqD", c, N);
        vDSP_convD(a, 1, b, 1, c, 1, N - 16, 16);   if (!r) mix("convD", c, N - 16);
        vDSP_mmulD(a, 1, b, 1, c, 1, 16, 16, 32);   if (!r) mix("mmulD", c, 256);
        memcpy(re, a, sizeof re);
        memcpy(imag, b, sizeof imag);
        z.realp = re; z.imagp = imag;
        vDSP_fft_zipD(fs, &z, 1, 10, kFFTDirection_Forward);
        if (!r) { mix("fft re", re, N); mix("fft im", imag, N); }
        vvsqrt(c, b, &n);                           if (!r) mix("vvsqrt", c, N);
        vvexp(c, a, &n);                            if (!r) mix("vvexp", c, N);
        vvlog(c, b, &n);                            if (!r) mix("vvlog", c, N);
        vvsin(c, a, &n);                            if (!r) mix("vvsin", c, N);
    }
    printf("total %08x\n", sum);
    vDSP_destroy_fftsetupD(fs);
    return 0;
}
