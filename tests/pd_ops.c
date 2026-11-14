/* pd_ops.c - packed doubles with special values (NaN, -0, infinity, denormals, overflow on conversion), every
 * operation over all value pairs, checksum of the result bits per operation. i386 only: comparison translated
 * against interpreter (ROSETTA_THRESHOLD=1 against ROSETTA_INTERP=1). */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <emmintrin.h>

static uint64_t vals[] = {
    0x0000000000000000ull, 0x8000000000000000ull, 0x3ff0000000000000ull, 0xbff0000000000000ull,
    0x3fe0000000000000ull, 0x4008000000000000ull, 0xc004000000000000ull, 0x7e37e43c8800759cull,
    0xfe37e43c8800759cull, 0x0000b8157268fdafull, 0x7ff0000000000000ull, 0xfff0000000000000ull,
    0x7ff8000000000000ull, 0xfff8000000000123ull, 0x7ff0000000000001ull, 0x41dfffffffe66666ull,
    0x41e0000000000000ull, 0xc1e0000000133333ull, 0xc1e0000000200000ull, 0x4012000000000000ull,
    0xc012000000000000ull, 0x3ff8000000000000ull, 0x3fd5555555555555ull, 0x7fefffffffffffffull,
};
#define NW ((int)(sizeof vals / sizeof vals[0]))

static double tab[2 * 24 + 8] __attribute__((aligned(16)));
static unsigned char raw[64 + 32] __attribute__((aligned(16)));
static int integer[8] __attribute__((aligned(16))) = { 7, -3, 2147483647, -2147483647 - 1, 0, 123456, -1, 42 };

static uint32_t sum[48];
static const char *names[48];

static void mix(int k, const char *name, __m128d v)
{
    union { __m128d v; uint8_t b[16]; } u;
    int i;
    u.v = v;
    names[k] = name;
    for (i = 0; i < 16; i++) sum[k] = (sum[k] ^ u.b[i]) * 16777619u;
}

static void mix_i(int k, const char *name, __m128i v) { mix(k, name, _mm_castsi128_pd(v)); }

static __m128d load(int i, int j)       /* (vals[i], vals[j]) as a pair via memory */
{
    memcpy(&tab[0], &vals[i], 8);
    memcpy(&tab[1], &vals[j], 8);
    return _mm_load_pd(tab);
}

/* x87 fistp in rounding mode cw (control word) */
static int fist(double v, unsigned short cw)
{
    int r;
    unsigned short prev;
    __asm__ volatile("fnstcw %0" : "=m"(prev));
    __asm__ volatile("fldcw %0" : : "m"(cw));
    __asm__ volatile("fistpl %0" : "=m"(r) : "t"(v) : "st");
    __asm__ volatile("fldcw %0" : : "m"(prev));
    return r;
}

int main(void)
{
    int i, j, k, o;
    for (k = 0; k < 48; k++) sum[k] = 2166136261u;
    for (i = 0; i < NW; i++) memcpy(&tab[8 + i], &vals[i], 8);
    for (i = 0; i < NW; i++)
        for (j = 0; j < NW; j++) {
            __m128d a = load(i, j), b = load(j, (i + 5) % NW), c, m;
            mix(0, "addpd", _mm_add_pd(a, b));
            mix(1, "subpd", _mm_sub_pd(a, b));
            mix(2, "mulpd", _mm_mul_pd(a, b));
            mix(3, "divpd", _mm_div_pd(a, b));
            mix(4, "minpd", _mm_min_pd(a, b));
            mix(5, "maxpd", _mm_max_pd(a, b));
            mix(6, "minsd", _mm_min_sd(a, b));
            mix(7, "maxsd", _mm_max_sd(a, b));
            mix(8, "sqrtpd", _mm_sqrt_pd(a));
            mix(9, "cmpeq", _mm_cmpeq_pd(a, b));
            mix(10, "cmplt", _mm_cmplt_pd(a, b));
            mix(11, "cmple", _mm_cmple_pd(a, b));
            mix(12, "cmpunord", _mm_cmpunord_pd(a, b));
            mix(13, "cmpneq", _mm_cmpneq_pd(a, b));
            mix(14, "cmpnlt", _mm_cmpnlt_pd(a, b));
            mix(15, "cmpnle", _mm_cmpnle_pd(a, b));
            mix(16, "cmpord", _mm_cmpord_pd(a, b));
            sum[17] = sum[17] * 31 + (uint32_t)_mm_movemask_pd(a) + 4 * (uint32_t)_mm_movemask_pd(_mm_cmplt_pd(a, b));
            names[17] = "movmskpd";
            mix_i(18, "cvttpd2dq", _mm_cvttpd_epi32(a));
            mix_i(19, "cvtpd2dq", _mm_cvtpd_epi32(b));
            mix(20, "unpcklpd", _mm_unpacklo_pd(a, b));
            mix(21, "unpckhpd", _mm_unpackhi_pd(a, b));
            mix(22, "shufpd", _mm_add_pd(_mm_add_pd(_mm_shuffle_pd(a, b, 0), _mm_shuffle_pd(a, b, 1)),
                                           _mm_sub_pd(_mm_shuffle_pd(a, b, 2), _mm_shuffle_pd(b, a, 3))));
            mix(23, "shufpd self", _mm_shuffle_pd(a, a, 1));
            mix(24, "and/or/xor", _mm_add_pd(_mm_xor_pd(_mm_and_pd(a, b), _mm_or_pd(a, b)), _mm_andnot_pd(a, b)));
            c = _mm_load_sd((double *)&vals[i]);           /* movsd: high half 0 */
            c = _mm_add_pd(c, b);
            mix(25, "movsd+addpd", c);
            m = _mm_move_sd(b, a);                          /* movsd between registers: high half stays */
            mix(26, "move_sd", _mm_mul_pd(m, a));
            mix(27, "loadh/l", _mm_loadh_pd(_mm_loadl_pd(a, (double *)&vals[j]), (double *)&vals[i]));
            o = (i + j) & 15;                               /* unaligned */
            _mm_storeu_pd((double *)(raw + o), _mm_add_pd(a, b));
            mix(28, "loadu/storeu", _mm_loadu_pd((double *)(raw + o)));
            _mm_storel_pd(&tab[2], a);
            _mm_storeh_pd(&tab[3], b);
            mix(29, "storel/h", _mm_load_pd(&tab[2]));
            mix(30, "movhl/lh",
                  _mm_castps_pd(_mm_movehl_ps(_mm_castpd_ps(a), _mm_movelh_ps(_mm_castpd_ps(b), _mm_castpd_ps(a)))));
            mix_i(31, "movq", _mm_add_epi64(_mm_move_epi64(_mm_castpd_si128(a)), _mm_loadl_epi64((__m128i *)&vals[j])));
            _mm_storel_epi64((__m128i *)&tab[4], _mm_castpd_si128(b));
            mix(32, "movq store", _mm_load_sd(&tab[4]));
            mix(33, "cvtdq2pd", _mm_mul_pd(_mm_cvtepi32_pd(_mm_loadl_epi64((__m128i *)&integer[(i + j) & 6])), a));
            mix(34, "xor self", _mm_add_pd(_mm_xor_pd(a, a), b));
            mix(35, "sqrt+max", _mm_max_pd(_mm_sqrt_pd(_mm_mul_pd(a, a)), _mm_set_pd(-0.0, 2.0)));
            sum[36] = sum[36] * 31 + (uint32_t)_mm_cvtsd_si32(a) + 7 * (uint32_t)_mm_cvttsd_si32(b);
            names[36] = "cvt(t)sd2si";
            mix(38, "cvtpd2ps", _mm_castps_pd(_mm_cvtpd_ps(_mm_mul_pd(a, b))));
            mix(39, "cvtps2pd", _mm_add_pd(_mm_cvtps_pd(_mm_cvtpd_ps(a)), _mm_cvtps_pd(_mm_castpd_ps(b))));
            mix(40, "cmpsd", _mm_add_pd(_mm_cmplt_sd(a, b), _mm_cmpunord_sd(b, a)));
            mix(41, "cmpsd+andpd", _mm_or_pd(_mm_and_pd(_mm_cmple_sd(a, b), a), _mm_andnot_pd(_mm_cmple_sd(a, b), b)));
        }
    names[37] = "fistp";
    for (i = 0; i < NW; i++) {
        double v;
        memcpy(&v, &vals[i], 8);
        for (k = 0; k < 4; k++) sum[37] = sum[37] * 31 + (uint32_t)fist(v, (unsigned short)(0x037f | k << 10));
    }
    for (k = 0; k < 42; k++) printf("%-16s %08x\n", names[k], sum[k]);
    return 0;
}
