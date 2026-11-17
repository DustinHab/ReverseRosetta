/* vec_ops.c - SSE2 integer and packed float operations (the AltiVec part of the translator) over all pairs of a set
 * of vectors with border values (0x7fff, 0x8000, -1, NaN, -0, infinity, denormals, values beyond 2^31, random bytes),
 * checksum of the result bits per operation. Comparison translated against interpreter (ROSETTA_THRESHOLD=1 against
 * ROSETTA_INTERP=1); the same program built natively on an Intel machine gives the reference checksums. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <emmintrin.h>

#define NV 24
static uint32_t vals[NV][4] __attribute__((aligned(16)));
static unsigned char unal[16 * NV + 16] __attribute__((aligned(16)));
static uint32_t sum[128];
static const char *names[128];
static int nops;

static uint32_t rnd(void)
{
    static uint32_t s = 2463534242u;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

static void setup(void)
{
    static const uint32_t f[] = {
        0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x3f000000, 0x3fc00000, 0x40200000, 0xc0200000,
        0x7fc00000, 0xffc00123, 0x7f800000, 0xff800000, 0x00000001, 0x807fffff, 0x7f7fffff, 0x4f000000,
        0xcf000000, 0x4effffff, 0x501502f9, 0xd01502f9, 0x406ccccd, 0xc06ccccd, 0x00800000, 0x3eaaaaab,
    };
    static const uint32_t w[] = {
        0x00000000, 0xffffffff, 0x7fff7fff, 0x80008000, 0x7fffffff, 0x80000000, 0x00ff00ff, 0xff00ff00,
        0x01020304, 0xfffefdfc, 0x12345678, 0x87654321, 0x00010001, 0x7f807f80, 0x80018001, 0x40004000,
    };
    int i, k;
    for (i = 0; i < NV; i++)
        for (k = 0; k < 4; k++) {
            if (i < 6) vals[i][k] = f[(i * 4 + k) % 24];                 /* floats */
            else if (i < 12) vals[i][k] = f[(i * 7 + k * 5) % 24];
            else if (i < 18) vals[i][k] = w[(i * 3 + k) % 16];             /* integer border values */
            else vals[i][k] = rnd();                                       /* random */
        }
    for (i = 0; i < (int)sizeof unal; i++) unal[i] = (unsigned char)rnd();
}

static void mix(int k, const char *name, __m128i v)
{
    union { __m128i v; uint8_t b[16]; } u;
    int i;
    u.v = v;
    names[k] = name;
    if (k >= nops) nops = k + 1;
    for (i = 0; i < 16; i++) sum[k] = (sum[k] ^ u.b[i]) * 16777619u;
}

static void mix_ps(int k, const char *name, __m128 v) { mix(k, name, _mm_castps_si128(v)); }
static void mix_w(int k, const char *name, uint32_t x) { mix(k, name, _mm_cvtsi32_si128((int)x)); }

#define A(i) (*(const __m128i *)vals[i])
#define F(i) (*(const __m128 *)vals[i])

static void pairs(void)
{
    int i, j;
    for (i = 0; i < NV; i++)
        for (j = 0; j < NV; j++) {
            __m128i a = A(i), b = A(j);
            __m128i c16 = _mm_and_si128(b, _mm_set_epi32(0, 0, 0, 31)), c32 = _mm_and_si128(b, _mm_set_epi32(0, 0, 0, 63));
            __m128 x = F(i), y = F(j);
            int k = 0;
            mix(k++, "paddb", _mm_add_epi8(a, b));
            mix(k++, "paddw", _mm_add_epi16(a, b));
            mix(k++, "paddd", _mm_add_epi32(a, b));
            mix(k++, "psubb", _mm_sub_epi8(a, b));
            mix(k++, "psubw", _mm_sub_epi16(a, b));
            mix(k++, "psubd", _mm_sub_epi32(a, b));
            mix(k++, "paddsb", _mm_adds_epi8(a, b));
            mix(k++, "paddsw", _mm_adds_epi16(a, b));
            mix(k++, "paddusb", _mm_adds_epu8(a, b));
            mix(k++, "paddusw", _mm_adds_epu16(a, b));
            mix(k++, "psubsb", _mm_subs_epi8(a, b));
            mix(k++, "psubsw", _mm_subs_epi16(a, b));
            mix(k++, "psubusb", _mm_subs_epu8(a, b));
            mix(k++, "psubusw", _mm_subs_epu16(a, b));
            mix(k++, "pmullw", _mm_mullo_epi16(a, b));
            mix(k++, "pmulhw", _mm_mulhi_epi16(a, b));
            mix(k++, "pmulhuw", _mm_mulhi_epu16(a, b));
            mix(k++, "pmaddwd", _mm_madd_epi16(a, b));
            mix(k++, "pavgb", _mm_avg_epu8(a, b));
            mix(k++, "pavgw", _mm_avg_epu16(a, b));
            mix(k++, "pminub", _mm_min_epu8(a, b));
            mix(k++, "pmaxub", _mm_max_epu8(a, b));
            mix(k++, "pminsw", _mm_min_epi16(a, b));
            mix(k++, "pmaxsw", _mm_max_epi16(a, b));
            mix(k++, "psadbw", _mm_sad_epu8(a, b));
            mix(k++, "pcmpgtb", _mm_cmpgt_epi8(a, b));
            mix(k++, "pcmpgtw", _mm_cmpgt_epi16(a, b));
            mix(k++, "pcmpgtd", _mm_cmpgt_epi32(a, b));
            mix(k++, "pcmpeqb", _mm_cmpeq_epi8(a, b));
            mix(k++, "pcmpeqw", _mm_cmpeq_epi16(a, b));
            mix(k++, "pcmpeqd", _mm_cmpeq_epi32(a, b));
            mix(k++, "pand", _mm_and_si128(a, b));
            mix(k++, "pandn", _mm_andnot_si128(a, b));
            mix(k++, "por", _mm_or_si128(a, b));
            mix(k++, "pxor", _mm_xor_si128(a, b));
            mix(k++, "packsswb", _mm_packs_epi16(a, b));
            mix(k++, "packuswb", _mm_packus_epi16(a, b));
            mix(k++, "packssdw", _mm_packs_epi32(a, b));
            mix(k++, "punpcklbw", _mm_unpacklo_epi8(a, b));
            mix(k++, "punpckhbw", _mm_unpackhi_epi8(a, b));
            mix(k++, "punpcklwd", _mm_unpacklo_epi16(a, b));
            mix(k++, "punpckhwd", _mm_unpackhi_epi16(a, b));
            mix(k++, "punpckldq", _mm_unpacklo_epi32(a, b));
            mix(k++, "punpckhdq", _mm_unpackhi_epi32(a, b));
            mix(k++, "punpcklqdq", _mm_unpacklo_epi64(a, b));
            mix(k++, "punpckhqdq", _mm_unpackhi_epi64(a, b));
            mix(k++, "psllw xmm", _mm_sll_epi16(a, c16));          /* shifts by a register: counts 0..31 / 0..63 */
            mix(k++, "psrlw xmm", _mm_srl_epi16(a, c16));
            mix(k++, "psraw xmm", _mm_sra_epi16(a, c16));
            mix(k++, "pslld xmm", _mm_sll_epi32(a, c32));
            mix(k++, "psrld xmm", _mm_srl_epi32(a, c32));
            mix(k++, "psrad xmm", _mm_sra_epi32(a, c32));
            mix(k++, "psrlw any", _mm_srl_epi16(a, b));             /* all 64 bits of the count */
            mix(k++, "psraw any", _mm_sra_epi16(a, b));
            mix_ps(k++, "addps", _mm_add_ps(x, y));
            mix_ps(k++, "subps", _mm_sub_ps(x, y));
            mix_ps(k++, "mulps", _mm_mul_ps(x, y));
            mix_ps(k++, "minps", _mm_min_ps(x, y));
            mix_ps(k++, "maxps", _mm_max_ps(x, y));
            mix_ps(k++, "unpcklps", _mm_unpacklo_ps(x, y));
            mix_ps(k++, "unpckhps", _mm_unpackhi_ps(x, y));
            mix_ps(k++, "shufps 1b", _mm_shuffle_ps(x, y, 0x1b));
            mix_ps(k++, "shufps e4", _mm_shuffle_ps(x, y, 0xe4));
            mix_ps(k++, "shufps 72", _mm_shuffle_ps(x, y, 0x72));
        }
}

static void singles(void)
{
    int i, k0 = 70;
    for (i = 0; i < NV; i++) {
        __m128i a = A(i);
        __m128 x = F(i);
        int k = k0;
        mix(k++, "psrlw 1", _mm_srli_epi16(a, 1));
        mix(k++, "psrlw 15", _mm_srli_epi16(a, 15));
        mix(k++, "psrlw 16", _mm_srli_epi16(a, 16));
        mix(k++, "psraw 3", _mm_srai_epi16(a, 3));
        mix(k++, "psraw 15", _mm_srai_epi16(a, 15));
        mix(k++, "psraw 20", _mm_srai_epi16(a, 20));
        mix(k++, "psllw 4", _mm_slli_epi16(a, 4));
        mix(k++, "psllw 16", _mm_slli_epi16(a, 16));
        mix(k++, "psrld 7", _mm_srli_epi32(a, 7));
        mix(k++, "psrld 20", _mm_srli_epi32(a, 20));
        mix(k++, "psrld 32", _mm_srli_epi32(a, 32));
        mix(k++, "psrad 1", _mm_srai_epi32(a, 1));
        mix(k++, "psrad 31", _mm_srai_epi32(a, 31));
        mix(k++, "psrad 40", _mm_srai_epi32(a, 40));
        mix(k++, "pslld 5", _mm_slli_epi32(a, 5));
        mix(k++, "pslld 17", _mm_slli_epi32(a, 17));
        mix(k++, "psrlq 1", _mm_srli_epi64(a, 1));
        mix(k++, "psrlq 32", _mm_srli_epi64(a, 32));
        mix(k++, "psrlq 45", _mm_srli_epi64(a, 45));
        mix(k++, "psrlq 64", _mm_srli_epi64(a, 64));
        mix(k++, "psllq 3", _mm_slli_epi64(a, 3));
        mix(k++, "psllq 32", _mm_slli_epi64(a, 32));
        mix(k++, "psllq 63", _mm_slli_epi64(a, 63));
        mix(k++, "psrldq 1", _mm_srli_si128(a, 1));
        mix(k++, "psrldq 8", _mm_srli_si128(a, 8));
        mix(k++, "psrldq 15", _mm_srli_si128(a, 15));
        mix(k++, "pslldq 3", _mm_slli_si128(a, 3));
        mix(k++, "pslldq 12", _mm_slli_si128(a, 12));
        mix(k++, "pshufd 1b", _mm_shuffle_epi32(a, 0x1b));
        mix(k++, "pshufd 4e", _mm_shuffle_epi32(a, 0x4e));
        mix(k++, "pshufd 00", _mm_shuffle_epi32(a, 0x00));
        mix(k++, "pshuflw b1", _mm_shufflelo_epi16(a, 0xb1));
        mix(k++, "pshufhw 1b", _mm_shufflehi_epi16(a, 0x1b));
        mix(k++, "cvtdq2ps", _mm_castps_si128(_mm_cvtepi32_ps(a)));
        mix(k++, "cvtps2dq", _mm_cvtps_epi32(x));
        mix(k++, "cvttps2dq", _mm_cvttps_epi32(x));
        mix_w(k++, "pextrw 0", (uint32_t)_mm_extract_epi16(a, 0));
        mix_w(k++, "pextrw 5", (uint32_t)_mm_extract_epi16(a, 5));
        mix(k++, "pinsrw 2", _mm_insert_epi16(a, (int)vals[(i + 5) % NV][1], 2));
        mix(k++, "pinsrw 7", _mm_insert_epi16(a, (int)vals[(i + 9) % NV][3], 7));
        mix_w(k++, "movd r32", (uint32_t)_mm_cvtsi128_si32(a));
        mix(k++, "movd xmm", _mm_cvtsi32_si128((int)vals[(i + 3) % NV][2]));
        mix(k++, "movdqu", _mm_loadu_si128((const __m128i *)(unal + 16 * i + (i % 15) + 1)));
        mix_ps(k++, "movups", _mm_loadu_ps((const float *)(unal + 16 * i + (i % 13) + 2)));
        mix_w(k++, "pmovmskb", (uint32_t)_mm_movemask_epi8(a));
        {
            uint32_t m32;
            __m128i t;
            union { __m128i v; uint32_t w[4]; } o __attribute__((aligned(16)));
            __asm__ volatile("movd %1, %0" : "=m"(m32) : "x"(a));
            mix_w(k++, "movd m32", m32);
            __asm__ volatile("movd %1, %0" : "=x"(t) : "m"(vals[(i + 7) % NV][3]));
            mix(k++, "movd from m32", t);
            __asm__ volatile("movdqu %1, %0" : "=m"(o.v) : "x"(a));
            mix(k++, "movdqu store", o.v);
            __asm__ volatile("movups %1, %0" : "=m"(o.v) : "x"(x));
            mix(k++, "movups store", o.v);
            __asm__ volatile("movntdq %1, %0" : "=m"(o.v) : "x"(a));
            mix(k++, "movntdq", o.v);
            t = a;
            __asm__ volatile("pinsrw $4, %1, %0" : "+x"(t) : "m"(vals[(i + 2) % NV][1]));
            mix(k++, "pinsrw m16", t);
        }
    }
}

int main(void)
{
    int k, round;
    setup();
    for (round = 0; round < 3; round++) { pairs(); singles(); }   /* several runs: translated after the threshold */
    for (k = 0; k < nops; k++) if (names[k]) printf("%-14s %08x\n", names[k], sum[k]);
    return 0;
}
