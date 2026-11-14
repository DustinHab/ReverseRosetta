/* sse_int.c - SSE2 integer instructions (movdqa/movdqu, pcmpeqb/w/d, pand/pandn/por/pxor, pmovmskb) against the
   same computation in C (PowerPC version); plus string functions of libSystem at all alignments. */
#include <stdio.h>
#include <string.h>
#ifdef __SSE2__
#include <emmintrin.h>
#endif

static unsigned h = 2166136261u;
static void m(unsigned v) { h = (h ^ v) * 16777619u; }

typedef struct { unsigned char b[16]; } V;

static void combine(const V *x, const V *y, V *r, int kind)
{
#ifdef __SSE2__
    __m128i a = _mm_loadu_si128((const __m128i *)x->b), c = _mm_loadu_si128((const __m128i *)y->b), e;
    switch (kind) {
    case 0: e = _mm_cmpeq_epi8(a, c); break;
    case 1: e = _mm_cmpeq_epi16(a, c); break;
    case 2: e = _mm_cmpeq_epi32(a, c); break;
    case 3: e = _mm_and_si128(a, c); break;
    case 4: e = _mm_andnot_si128(a, c); break;
    case 5: e = _mm_or_si128(a, c); break;
    default: e = _mm_xor_si128(a, c); break;
    }
    _mm_storeu_si128((__m128i *)r->b, e);
    m((unsigned)_mm_movemask_epi8(e));
#else
    int i;
    unsigned msk = 0;
    for (i = 0; i < 16; i++) {
        unsigned char u = x->b[i], v = y->b[i], w;
        switch (kind) {
        case 0: w = u == v ? 0xff : 0; break;
        case 1: w = (x->b[i & ~1] == y->b[i & ~1] && x->b[i | 1] == y->b[i | 1]) ? 0xff : 0; break;
        case 2: w = memcmp(x->b + (i & ~3), y->b + (i & ~3), 4) ? 0 : 0xff; break;
        case 3: w = u & v; break;
        case 4: w = (unsigned char)(~u & v); break;
        case 5: w = u | v; break;
        default: w = u ^ v; break;
        }
        r->b[i] = w;
        if (w & 0x80) msk |= 1u << i;
    }
    m(msk);
#endif
}

int main(void)
{
    static V vals[6];
    static char text[300];
    V r;
    int i, j, k, kind;
    for (i = 0; i < 6; i++)
        for (k = 0; k < 16; k++) vals[i].b[k] = (unsigned char)(i * 37 + k * (i + 3) + (k & 1 ? 0x80 : 0));
    vals[1] = vals[0];
    vals[2].b[3] = vals[0].b[3]; vals[2].b[4] = vals[0].b[4];
    for (i = 0; i < 6; i++)
        for (j = 0; j < 6; j++)
            for (kind = 0; kind < 7; kind++) {
                combine(&vals[i], &vals[j], &r, kind);
                for (k = 0; k < 16; k++) m(r.b[k]);
            }
    printf("%08x ", h);
    /* strings: strlen/strchr/memchr/strcmp at all start and end positions */
    for (i = 0; i < 64; i++)
        for (j = 0; j < 70; j++) {
            memset(text, 'a' + (i + j) % 26, sizeof text);
            text[i + j] = 0;
            m((unsigned)strlen(text + i));
            text[i + j / 2] = 'Z';
            m((unsigned)(strchr(text + i, 'Z') ? strchr(text + i, 'Z') - text : -1));
            m((unsigned)(memchr(text + i, 'Z', (size_t)j) ? 1 : 0));
            m((unsigned)(strcmp(text + i, text + 1) > 0));
        }
    printf("%08x\n", h);
    return 0;
}
