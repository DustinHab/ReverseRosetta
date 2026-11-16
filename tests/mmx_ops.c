/* mmx_ops.c - MMX instructions (64-bit integer operations on mm0-mm7, as QuickTime's codecs use them): every operation
 * on a table of operand pairs with border values, one checksum per operation. Reference: the same source built with
 * gcc for x86-64 on a real x86 processor (tests/mmx_ops.expected); the translator's output must be identical. Built with
 * -Os: Apple's gcc-4.2 fails at -O1/-O2 (unrecognizable insn). */
#include <stdio.h>
#include <string.h>
#include <mmintrin.h>
#include <xmmintrin.h>

static const unsigned long long g_v[] = {
    0ull, 1ull, 0xffffffffffffffffull, 0x8000000000000000ull, 0x7fffffffffffffffull, 0x0123456789abcdefull,
    0xfedcba9876543210ull, 0x8080808080808080ull, 0x7f7f7f7f7f7f7f7full, 0x00ff00ff00ff00ffull, 0xff00ff00ff00ff00ull,
    0x8000800080008000ull, 0x7fff7fff7fff7fffull, 0x0000ffff0000ffffull, 0x80000000ffffffffull, 0x123456789abcdef0ull,
};
#define NV (int)(sizeof g_v / sizeof g_v[0])

static __m64 mk(unsigned long long x) { __m64 m; memcpy(&m, &x, 8); return m; }
static unsigned long long un(__m64 m) { unsigned long long x; memcpy(&x, &m, 8); return x; }
static unsigned long long g_sum;
static void mix(unsigned long long x) { g_sum = (g_sum ^ x) * 0x100000001b3ull + (g_sum >> 29); }

#define BIN(name, expr)                                                                        \
    do {                                                                                       \
        int i_, j_;                                                                            \
        g_sum = 0;                                                                             \
        for (i_ = 0; i_ < NV; i_++)                                                            \
            for (j_ = 0; j_ < NV; j_++) {                                                      \
                __m64 a = mk(g_v[i_]), b = mk(g_v[j_]);                                        \
                mix(un(expr));                                                                 \
            }                                                                                  \
        _mm_empty();                                                                           \
        printf("%-12s %016llx\n", name, g_sum);                                                \
    } while (0)

#define UN(name, expr)                                                                         \
    do {                                                                                       \
        int i_;                                                                                \
        g_sum = 0;                                                                             \
        for (i_ = 0; i_ < NV; i_++) { __m64 a = mk(g_v[i_]); mix(un(expr)); }                   \
        _mm_empty();                                                                           \
        printf("%-12s %016llx\n", name, g_sum);                                                \
    } while (0)

int main(void)
{
    BIN("paddb", _mm_add_pi8(a, b)); BIN("paddw", _mm_add_pi16(a, b)); BIN("paddd", _mm_add_pi32(a, b));
    BIN("paddsb", _mm_adds_pi8(a, b)); BIN("paddsw", _mm_adds_pi16(a, b));
    BIN("paddusb", _mm_adds_pu8(a, b)); BIN("paddusw", _mm_adds_pu16(a, b));
    BIN("psubb", _mm_sub_pi8(a, b)); BIN("psubw", _mm_sub_pi16(a, b)); BIN("psubd", _mm_sub_pi32(a, b));
    BIN("psubsb", _mm_subs_pi8(a, b)); BIN("psubsw", _mm_subs_pi16(a, b));
    BIN("psubusb", _mm_subs_pu8(a, b)); BIN("psubusw", _mm_subs_pu16(a, b));
    BIN("pmullw", _mm_mullo_pi16(a, b)); BIN("pmulhw", _mm_mulhi_pi16(a, b)); BIN("pmulhuw", _mm_mulhi_pu16(a, b));
    BIN("pmaddwd", _mm_madd_pi16(a, b));
    BIN("pand", _mm_and_si64(a, b)); BIN("pandn", _mm_andnot_si64(a, b)); BIN("por", _mm_or_si64(a, b));
    BIN("pxor", _mm_xor_si64(a, b));
    BIN("pcmpeqb", _mm_cmpeq_pi8(a, b)); BIN("pcmpeqw", _mm_cmpeq_pi16(a, b)); BIN("pcmpeqd", _mm_cmpeq_pi32(a, b));
    BIN("pcmpgtb", _mm_cmpgt_pi8(a, b)); BIN("pcmpgtw", _mm_cmpgt_pi16(a, b)); BIN("pcmpgtd", _mm_cmpgt_pi32(a, b));
    BIN("punpcklbw", _mm_unpacklo_pi8(a, b)); BIN("punpcklwd", _mm_unpacklo_pi16(a, b));
    BIN("punpckldq", _mm_unpacklo_pi32(a, b)); BIN("punpckhbw", _mm_unpackhi_pi8(a, b));
    BIN("punpckhwd", _mm_unpackhi_pi16(a, b)); BIN("punpckhdq", _mm_unpackhi_pi32(a, b));
    BIN("packsswb", _mm_packs_pi16(a, b)); BIN("packssdw", _mm_packs_pi32(a, b)); BIN("packuswb", _mm_packs_pu16(a, b));
    BIN("psllw", _mm_sll_pi16(a, _mm_and_si64(b, mk(31)))); BIN("pslld", _mm_sll_pi32(a, _mm_and_si64(b, mk(63))));
    BIN("psllq", _mm_sll_si64(a, _mm_and_si64(b, mk(127)))); BIN("psrlw", _mm_srl_pi16(a, _mm_and_si64(b, mk(31))));
    BIN("psrld", _mm_srl_pi32(a, _mm_and_si64(b, mk(63)))); BIN("psrlq", _mm_srl_si64(a, _mm_and_si64(b, mk(127))));
    BIN("psraw", _mm_sra_pi16(a, _mm_and_si64(b, mk(31)))); BIN("psrad", _mm_sra_pi32(a, _mm_and_si64(b, mk(63))));
    UN("psllw 3", _mm_slli_pi16(a, 3)); UN("pslld 17", _mm_slli_pi32(a, 17)); UN("psllq 33", _mm_slli_si64(a, 33));
    UN("psrlw 5", _mm_srli_pi16(a, 5)); UN("psrld 31", _mm_srli_pi32(a, 31)); UN("psrlq 7", _mm_srli_si64(a, 7));
    UN("psraw 15", _mm_srai_pi16(a, 15)); UN("psrad 4", _mm_srai_pi32(a, 4));
    BIN("pavgb", _mm_avg_pu8(a, b)); BIN("pavgw", _mm_avg_pu16(a, b));
    BIN("pminub", _mm_min_pu8(a, b)); BIN("pmaxub", _mm_max_pu8(a, b));
    BIN("pminsw", _mm_min_pi16(a, b)); BIN("pmaxsw", _mm_max_pi16(a, b));
    BIN("psadbw", _mm_sad_pu8(a, b));
    UN("pshufw 1b", _mm_shuffle_pi16(a, 0x1b)); UN("pshufw e4", _mm_shuffle_pi16(a, 0xe4));
    UN("pshufw 5c", _mm_shuffle_pi16(a, 0x5c));
    {                                   /* results in general registers (Apple's gcc-4.2 fails on them inside mk()) */
        int i;
        unsigned long long s1 = 0, s2 = 0, s3 = 0;
        for (i = 0; i < NV; i++) {
            __m64 a = mk(g_v[i]);
            int m = _mm_movemask_pi8(a), e = _mm_extract_pi16(a, 2), d = _mm_cvtsi64_si32(a);
            g_sum = s1; mix((unsigned)m); s1 = g_sum;
            g_sum = s2; mix((unsigned)e & 0xffff); s2 = g_sum;   /* (gcc-4.2 sign-extends the word) */
            g_sum = s3; mix((unsigned)d); s3 = g_sum;
        }
        _mm_empty();
        printf("%-12s %016llx\n%-12s %016llx\n", "pmovmskb", s1, "pextrw 2", s2);
        UN("pinsrw 3", _mm_insert_pi16(a, 0xbeef, 3));
        printf("%-12s %016llx\n", "movd", s3);
    }
    UN("movd to mm", _mm_cvtsi32_si64((int)(un(a) >> 7)));
    {
        int i;
        g_sum = 0;
        for (i = 0; i < NV; i++) {      /* cvtpi2ps, cvtps2pi, cvttps2pi */
            __m64 a = mk(g_v[i] & 0x00ffffff00ffffffull);
            __m128 f = _mm_cvtpi32_ps(_mm_setzero_ps(), a);
            __m128 g = _mm_mul_ps(f, _mm_set1_ps(0.75f));
            mix(un(_mm_cvtps_pi32(g)));
            mix(un(_mm_cvttps_pi32(g)));
        }
        _mm_empty();
        printf("%-12s %016llx\n", "cvtpi/ps", g_sum);
    }
    return 0;
}
