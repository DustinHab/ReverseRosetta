/* bench_sse2.c [ROUNDS] - packed SSE2 double: daxpy, dot product (two lanes), complex multiplication (shufpd,
 * unpcklpd/unpckhpd, xorpd), normalization (sqrtpd, divpd), clamping (minpd, maxpd), compare with mask (cmppd,
 * andpd, movmskpd), conversion (cvtdq2pd, cvttpd2dq). On PowerPC the same computations lane by lane in scalar code:
 * the results must be bit-identical (no contraction to fmadd: -mno-fused-madd). */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define N 1024

#if defined(__SSE2__)
#include <emmintrin.h>
typedef __m128d V;
#define LOAD(p) _mm_load_pd(p)
#define LOADU(p) _mm_loadu_pd(p)
#define STORE(p, v) _mm_store_pd(p, v)
#define ADD(a, b) _mm_add_pd(a, b)
#define SUB(a, b) _mm_sub_pd(a, b)
#define MUL(a, b) _mm_mul_pd(a, b)
#define DIV(a, b) _mm_div_pd(a, b)
#define ROOT(a) _mm_sqrt_pd(a)
#define MIN(a, b) _mm_min_pd(a, b)
#define MAX(a, b) _mm_max_pd(a, b)
#define BOTH(x) _mm_set1_pd(x)
#define PAIR(lo, hi) _mm_set_pd(hi, lo)
#define BOTTOM(a, b) _mm_unpacklo_pd(a, b)
#define TOP(a, b) _mm_unpackhi_pd(a, b)
#define XCHG(a) _mm_shuffle_pd(a, a, 1)
#define XOR(a, b) _mm_xor_pd(a, b)
#define AND(a, b) _mm_and_pd(a, b)
#define LESS(a, b) _mm_cmplt_pd(a, b)
#define MASK_BITS(a) _mm_movemask_pd(a)
#define LO(a) _mm_cvtsd_f64(a)
#define HI(a) _mm_cvtsd_f64(_mm_unpackhi_pd(a, a))
#else
typedef struct { double l, h; } V;
static inline V pair(double l, double h) { V v; v.l = l; v.h = h; return v; }
#define LOAD(p) pair((p)[0], (p)[1])
#define LOADU(p) pair((p)[0], (p)[1])
#define STORE(p, v) do { V w_ = (v); (p)[0] = w_.l; (p)[1] = w_.h; } while (0)
static inline V ADD(V a, V b) { return pair(a.l + b.l, a.h + b.h); }
static inline V SUB(V a, V b) { return pair(a.l - b.l, a.h - b.h); }
static inline V MUL(V a, V b) { return pair(a.l * b.l, a.h * b.h); }
static inline V DIV(V a, V b) { return pair(a.l / b.l, a.h / b.h); }
static inline V ROOT(V a) { return pair(sqrt(a.l), sqrt(a.h)); }
static inline double min1(double a, double b) { return a < b ? a : b; }   /* minpd: a < b ? a : b */
static inline double max1(double a, double b) { return a > b ? a : b; }
static inline V MIN(V a, V b) { return pair(min1(a.l, b.l), min1(a.h, b.h)); }
static inline V MAX(V a, V b) { return pair(max1(a.l, b.l), max1(a.h, b.h)); }
#define BOTH(x) pair(x, x)
#define PAIR(lo, hi) pair(lo, hi)
static inline V BOTTOM(V a, V b) { return pair(a.l, b.l); }
static inline V TOP(V a, V b) { return pair(a.h, b.h); }
static inline V XCHG(V a) { return pair(a.h, a.l); }
static inline V XORNEG(V a, int l, int h) { return pair(l ? -a.l : a.l, h ? -a.h : a.h); }
static inline int MASK_LT(V a, V b) { return (a.l < b.l) | (a.h < b.h) << 1; }
#define LO(a) ((a).l)
#define HI(a) ((a).h)
#endif

static double x[N] __attribute__((aligned(16))), y[N] __attribute__((aligned(16))), z[N] __attribute__((aligned(16)));
static int integer[N] __attribute__((aligned(16)));

static double chk(const double *a, int n)
{
    double s = 0;
    int i;
    for (i = 0; i < n; i++) s += a[i] * (double)(i % 7 + 1);
    return s;
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 10, r, i, hits = 0;
    double dot = 0, kmp = 0;
    for (i = 0; i < N; i++) { x[i] = (i % 13) * 0.25 + 1.0 / (i + 1); y[i] = (i % 17) * 0.5 - 3.0; z[i] = 0; integer[i] = i * 7 - 2000; }
    for (r = 0; r < rounds; r++) {
        V a = BOTH(1.0 + r * 1e-3), s0 = BOTH(0.0), s1 = BOTH(0.0), lo = BOTH(-4.0), hi = BOTH(4.0);
        for (i = 0; i < N; i += 2) STORE(&y[i], ADD(LOAD(&y[i]), MUL(a, LOAD(&x[i]))));        /* daxpy */
        for (i = 0; i < N; i += 4) {                                                                    /* dot product */
            s0 = ADD(s0, MUL(LOAD(&x[i]), LOAD(&y[i])));
            s1 = ADD(s1, MUL(LOAD(&x[i + 2]), LOAD(&y[i + 2])));
        }
        s0 = ADD(s0, s1);
        dot += LO(s0) + HI(s0);
        for (i = 0; i < N; i += 2) {        /* complex: (xr + i xi) * (yr + i yi) as pairs (r, i) */
            V p = LOAD(&x[i]), q = LOAD(&y[i]);
            V re = MUL(BOTTOM(p, p), q);                         /* (xr*yr, xr*yi) */
            V imag = MUL(TOP(p, p), XCHG(q));                  /* (xi*yi, xi*yr) */
#if defined(__SSE2__)
            imag = XOR(imag, _mm_set_pd(0.0, -0.0));                /* (-xi*yi, xi*yr) */
#else
            imag = XORNEG(imag, 1, 0);
#endif
            STORE(&z[i], ADD(re, imag));
        }
        kmp += chk(z, N);
        for (i = 0; i < N; i += 2) {        /* normalize and clamp */
            V p = LOAD(&z[i]), q = LOAD(&x[i]);
            V n = ROOT(ADD(MUL(p, p), MUL(q, q)));
            STORE(&z[i], MAX(lo, MIN(hi, DIV(p, ADD(n, BOTH(1.0))))));
        }
        for (i = 0; i < N; i += 2) {        /* compare with mask */
#if defined(__SSE2__)
            V m = LESS(LOAD(&z[i]), BOTH(0.25));
            hits += __builtin_popcount(MASK_BITS(m));
            STORE(&y[i], ADD(LOAD(&y[i]), AND(m, BOTH(0.125))));
#else
            V v = LOAD(&z[i]);
            int m = MASK_LT(v, BOTH(0.25));
            hits += (m & 1) + (m >> 1);
            y[i] += (m & 1) ? 0.125 : 0.0;
            y[i + 1] += (m & 2) ? 0.125 : 0.0;
#endif
        }
        for (i = 0; i < N; i += 2) {        /* integer -> double -> integer */
#if defined(__SSE2__)
            V d = _mm_cvtepi32_pd(_mm_loadl_epi64((const __m128i *)&integer[i]));
            d = MUL(d, BOTH(0.5));
            _mm_storel_epi64((__m128i *)&integer[i], _mm_cvttpd_epi32(ADD(d, BOTH(0.75))));
#else
            integer[i] = (int)((double)integer[i] * 0.5 + 0.75);
            integer[i + 1] = (int)((double)integer[i + 1] * 0.5 + 0.75);
#endif
            integer[i] = integer[i] * 2 + 1;
            integer[i + 1] = integer[i + 1] * 2 - 1;
        }
    }
    {
        long gs = 0;
        for (i = 0; i < N; i++) gs += integer[i];
        printf("dot %.17g\ncomplex %.17g\ny %.17g\nz %.17g\nhits %d\nint %ld\n", dot, kmp, chk(y, N), chk(z, N), hits, gs);
    }
    return 0;
}
