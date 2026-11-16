/* tec.c - the Text Encoding Converter (TEC*) of the system's PowerPC CarbonCore for Intel programs.
 *
 * TECCreateConverter and the rest of the converter API load the plugins in /System/Library/TextEncodings, which exist
 * only for PowerPC: under rosetta every converter failed (-8770, even UTF-8 to MacRoman) and wxWidgets programs
 * stopped at an assertion. thunk.c replaces these functions of the guest's CarbonCore image by name with the ones here
 * (tec_stub); TextToUnicodeInfo and CFString conversions keep working in i386 code. Converters and sniffers are host
 * objects passed as they are; counts and encodings are 32-bit words. Unicode in the host byte order (16-bit and 32-bit
 * formats without an explicit BE/LE, kUnicode16BitFormat and kUnicode32BitFormat and the UTF16/UTF32 ones) is swapped
 * between the guest's little-endian and the host's big-endian units on the way in and out. */
#include "tec.h"
#include "cpu.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define A(k) ld32(c->r[ESP] + 4 + 4 * (k))
#define RET(v) do { c->r[EAX] = (uint32_t)(v); tec_log(c, __func__); return 0; } while (0)

static int g_tec_log = -1;
static void tec_log(Cpu *c, const char *f)
{
    if (g_tec_log < 0) g_tec_log = getenv("ROSETTA_TEC_LOG") != 0;
    if (g_tec_log) fprintf(stderr, "[tec] %s(%08x %08x %08x %08x) = %d\n", f + 2, A(0), A(1), A(2), A(3), (int)c->r[EAX]);
}

static void *g_cc;
static void *tec_sym(const char *n)
{
    void *f;
    if (!g_cc && !(g_cc = dlopen("/System/Library/Frameworks/CoreServices.framework/Frameworks/CarbonCore.framework/CarbonCore",
                                 RTLD_LAZY | RTLD_LOCAL))) {
        fprintf(stderr, "rosetta: CarbonCore: %s\n", dlerror());
        return 0;
    }
    if (!(f = dlsym(g_cc, n))) fprintf(stderr, "rosetta: CarbonCore: no %s on this system\n", n);
    return f;
}
static void *tec_set(void **p, const char *n) { return *p = tec_sym(n); }
#define SYM(n) (f_##n ? f_##n : tec_set(&f_##n, #n))
#define F(n) static void *f_##n
F(TECCreateConverter); F(TECCreateConverterFromPath); F(TECDisposeConverter); F(TECClearConverterContextInfo);
F(TECConvertText); F(TECFlushText); F(TECSetBasicOptions); F(TECGetTextEncodingInternetName);
F(TECGetTextEncodingFromInternetName); F(TECCountAvailableTextEncodings); F(TECGetAvailableTextEncodings);
F(TECCountDirectTextEncodingConversions); F(TECGetDirectTextEncodingConversions); F(TECCountDestinationTextEncodings);
F(TECGetDestinationTextEncodings); F(TECCreateSniffer); F(TECSniffTextEncoding); F(TECDisposeSniffer);
F(TECClearSnifferContextInfo); F(TECCountMailTextEncodings); F(TECGetMailTextEncodings);
F(TECCountWebTextEncodings); F(TECGetWebTextEncodings);
#undef F
typedef int32_t St;

/* unit to swap for an encoding: 2 or 4 for Unicode in the host's byte order, else 0 */
static int unit(uint32_t enc)
{
    uint32_t base = enc & 0xffff, format = (enc >> 26) & 0x3f;
    if (base < 0x100 || base > 0x1ff) return 0;
    switch (format) {
    case 0: case 8: return 2;               /* kUnicode16BitFormat, kUnicodeUTF16Format */
    case 3: case 9: return 4;               /* kUnicode32BitFormat, kUnicodeUTF32Format */
    }
    return 0;
}

static void swap_units(uint8_t *p, uint32_t n, int u)
{
    uint32_t k;
    if (u == 2) for (k = 0; k + 2 <= n; k += 2) { uint8_t t = p[k]; p[k] = p[k + 1]; p[k + 1] = t; }
    else if (u == 4) for (k = 0; k + 4 <= n; k += 4) *(uint32_t *)(void *)(p + k) = swap32(*(uint32_t *)(void *)(p + k));
}

/* converters and the units of their input and output */
#define CONV_N 256
static struct { uint32_t ref; int in, out; } g_conv[CONV_N];

static void conv_note(uint32_t ref, uint32_t in, uint32_t out)
{
    int k, free_k = -1;
    for (k = 0; k < CONV_N; k++) {
        if (g_conv[k].ref == ref) { free_k = k; break; }
        if (!g_conv[k].ref && free_k < 0) free_k = k;
    }
    if (free_k < 0) free_k = (int)(ref >> 4) % CONV_N;
    g_conv[free_k].ref = ref; g_conv[free_k].in = unit(in); g_conv[free_k].out = unit(out);
}

static int conv_find(uint32_t ref)
{
    int k;
    for (k = 0; k < CONV_N; k++) if (g_conv[k].ref == ref) return k;
    return -1;
}

static uint8_t *g_tmp;
static uint32_t g_tmp_size;
static uint8_t *tmp(uint32_t n)
{
    if (n > (1u << 26)) return 0;
    if (n > g_tmp_size) {
        void *p = realloc(g_tmp, n ? n : 1);
        if (!p) return 0;
        g_tmp = p;
        g_tmp_size = n;
    }
    return g_tmp;
}

/* encodings: a guest array of n words -> host array (in the scratch buffer) */
static uint32_t *enc_in(uint32_t a, uint32_t n)
{
    uint32_t *t = (uint32_t *)(void *)tmp(4 * n + 4), k;
    if (t) for (k = 0; k < n; k++) t[k] = ld32(a + 4 * k);
    return t;
}

static int c_TECCreateConverter(Cpu *c)
{
    uint32_t ref = 0;
    St e = ((St (*)(uint32_t *, uint32_t, uint32_t))SYM(TECCreateConverter))(&ref, A(1), A(2));
    if (!e) { st32(A(0), ref); conv_note(ref, A(1), A(2)); }
    RET(e);
}

static int c_TECCreateConverterFromPath(Cpu *c)
{
    uint32_t ref = 0, n = A(2), *p = enc_in(A(1), n), first, last;
    St e;
    if (!p || !n) RET(-50);
    first = p[0]; last = p[n - 1];
    e = ((St (*)(uint32_t *, const uint32_t *, uint32_t))SYM(TECCreateConverterFromPath))(&ref, p, n);
    if (!e) { st32(A(0), ref); conv_note(ref, first, last); }
    RET(e);
}

static int c_TECDisposeConverter(Cpu *c)
{
    int k = conv_find(A(0));
    if (k >= 0) g_conv[k].ref = 0;
    RET(((St (*)(uint32_t))SYM(TECDisposeConverter))(A(0)));
}

static int c_TECClearConverterContextInfo(Cpu *c) { RET(((St (*)(uint32_t))SYM(TECClearConverterContextInfo))(A(0))); }
static int c_TECSetBasicOptions(Cpu *c) { RET(((St (*)(uint32_t, uint32_t))SYM(TECSetBasicOptions))(A(0), A(1))); }

static int c_TECConvertText(Cpu *c)
{
    int k = conv_find(A(0)), ui = k >= 0 ? g_conv[k].in : 0, uo = k >= 0 ? g_conv[k].out : 0;
    uint32_t inlen = A(2), rd = 0, wr = 0;
    const uint8_t *in = (const uint8_t *)(uintptr_t)A(1);
    St e;
    if (ui) {
        uint8_t *t = tmp(inlen);
        if (!t) RET(-108);
        memcpy(t, in, inlen);
        swap_units(t, inlen, ui);
        in = t;
    }
    e = ((St (*)(uint32_t, const uint8_t *, uint32_t, uint32_t *, uint8_t *, uint32_t, uint32_t *))SYM(TECConvertText))(
        A(0), in, inlen, &rd, (uint8_t *)(uintptr_t)A(4), A(5), &wr);
    if (uo) swap_units((uint8_t *)(uintptr_t)A(4), wr, uo);
    if (A(3)) st32(A(3), rd);
    if (A(6)) st32(A(6), wr);
    RET(e);
}

static int c_TECFlushText(Cpu *c)
{
    int k = conv_find(A(0)), uo = k >= 0 ? g_conv[k].out : 0;
    uint32_t wr = 0;
    St e = ((St (*)(uint32_t, uint8_t *, uint32_t, uint32_t *))SYM(TECFlushText))(A(0), (uint8_t *)(uintptr_t)A(1), A(2), &wr);
    if (uo) swap_units((uint8_t *)(uintptr_t)A(1), wr, uo);
    if (A(3)) st32(A(3), wr);
    RET(e);
}

static int c_TECGetTextEncodingInternetName(Cpu *c)
{
    RET(((St (*)(uint32_t, uint8_t *))SYM(TECGetTextEncodingInternetName))(A(0), (uint8_t *)(uintptr_t)A(1)));
}

static int c_TECGetTextEncodingFromInternetName(Cpu *c)
{
    uint32_t enc = 0;
    St e = ((St (*)(uint32_t *, const uint8_t *))SYM(TECGetTextEncodingFromInternetName))(&enc, (const uint8_t *)(uintptr_t)A(1));
    if (!e) st32(A(0), enc);
    RET(e);
}

/* count functions: (args..., ItemCount *) */
static int c_TECCountAvailableTextEncodings(Cpu *c)
{
    uint32_t n = 0;
    St e = ((St (*)(uint32_t *))SYM(TECCountAvailableTextEncodings))(&n);
    if (!e) st32(A(0), n);
    RET(e);
}

static int c_TECCountDirectTextEncodingConversions(Cpu *c)
{
    uint32_t n = 0;
    St e = ((St (*)(uint32_t *))SYM(TECCountDirectTextEncodingConversions))(&n);
    if (!e) st32(A(0), n);
    RET(e);
}

static int c_TECCountDestinationTextEncodings(Cpu *c)
{
    uint32_t n = 0;
    St e = ((St (*)(uint32_t, uint32_t *))SYM(TECCountDestinationTextEncodings))(A(0), &n);
    if (!e) st32(A(1), n);
    RET(e);
}

static int c_TECCountMailTextEncodings(Cpu *c)
{
    uint32_t n = 0;
    St e = ((St (*)(uint32_t, uint32_t *))SYM(TECCountMailTextEncodings))(A(0), &n);
    if (!e) st32(A(1), n);
    RET(e);
}

static int c_TECCountWebTextEncodings(Cpu *c)
{
    uint32_t n = 0;
    St e = ((St (*)(uint32_t, uint32_t *))SYM(TECCountWebTextEncodings))(A(0), &n);
    if (!e) st32(A(1), n);
    RET(e);
}

/* get functions: (args..., array, max, ItemCount *actual); words */
static St get_words(void *f, int lead, uint32_t lead_arg, uint32_t arr, uint32_t max, uint32_t actual)
{
    uint32_t *t = (uint32_t *)(void *)tmp(4 * max + 4), n = 0, k;
    St e;
    if (!t) return -108;
    e = lead ? ((St (*)(uint32_t, uint32_t *, uint32_t, uint32_t *))f)(lead_arg, t, max, &n)
             : ((St (*)(uint32_t *, uint32_t, uint32_t *))f)(t, max, &n);
    if (!e) {
        for (k = 0; k < n && k < max; k++) st32(arr + 4 * k, t[k]);
        if (actual) st32(actual, n);
    }
    return e;
}

static int c_TECGetAvailableTextEncodings(Cpu *c) { RET(get_words(SYM(TECGetAvailableTextEncodings), 0, 0, A(0), A(1), A(2))); }
static int c_TECGetDestinationTextEncodings(Cpu *c) { RET(get_words(SYM(TECGetDestinationTextEncodings), 1, A(0), A(1), A(2), A(3))); }
static int c_TECGetMailTextEncodings(Cpu *c) { RET(get_words(SYM(TECGetMailTextEncodings), 1, A(0), A(1), A(2), A(3))); }
static int c_TECGetWebTextEncodings(Cpu *c) { RET(get_words(SYM(TECGetWebTextEncodings), 1, A(0), A(1), A(2), A(3))); }

static int c_TECGetDirectTextEncodingConversions(Cpu *c)
{
    /* TECConversionInfo: source, destination (words), two reserved 16-bit fields */
    uint32_t max = A(1), n = 0, k;
    uint8_t *t = tmp(12 * max + 12);
    St e;
    if (!t) RET(-108);
    e = ((St (*)(uint8_t *, uint32_t, uint32_t *))SYM(TECGetDirectTextEncodingConversions))(t, max, &n);
    if (!e) {
        for (k = 0; k < n && k < max; k++) {
            st32(A(0) + 12 * k, *(uint32_t *)(void *)(t + 12 * k));
            st32(A(0) + 12 * k + 4, *(uint32_t *)(void *)(t + 12 * k + 4));
            st16(A(0) + 12 * k + 8, *(uint16_t *)(void *)(t + 12 * k + 8));
            st16(A(0) + 12 * k + 10, *(uint16_t *)(void *)(t + 12 * k + 10));
        }
        if (A(2)) st32(A(2), n);
    }
    RET(e);
}

static int c_TECCreateSniffer(Cpu *c)
{
    uint32_t ref = 0, *p = enc_in(A(1), A(2));
    St e;
    if (!p) RET(-108);
    e = ((St (*)(uint32_t *, const uint32_t *, uint32_t))SYM(TECCreateSniffer))(&ref, p, A(2));
    if (!e) st32(A(0), ref);
    RET(e);
}

static int c_TECSniffTextEncoding(Cpu *c)
{
    /* (sniffer, input, length, encodings[], n, errors[], maxErrs, features[], maxFeatures): the encodings are reordered
       by the result, the two count arrays are written */
    uint32_t n = A(4), k, me = A(6), mf = A(8);
    uint32_t *enc = malloc(4 * n + 4), *errs = malloc(4 * me + 4), *feat = malloc(4 * mf + 4);
    St e = -108;
    if (enc && errs && feat) {
        for (k = 0; k < n; k++) enc[k] = ld32(A(3) + 4 * k);
        for (k = 0; k < me; k++) errs[k] = ld32(A(5) + 4 * k);
        for (k = 0; k < mf; k++) feat[k] = ld32(A(7) + 4 * k);
        e = ((St (*)(uint32_t, const void *, uint32_t, uint32_t *, uint32_t, uint32_t *, uint32_t, uint32_t *, uint32_t))
             SYM(TECSniffTextEncoding))(A(0), (const void *)(uintptr_t)A(1), A(2), enc, n, errs, me, feat, mf);
        for (k = 0; k < n; k++) st32(A(3) + 4 * k, enc[k]);
        for (k = 0; k < me; k++) st32(A(5) + 4 * k, errs[k]);
        for (k = 0; k < mf; k++) st32(A(7) + 4 * k, feat[k]);
    }
    free(enc); free(errs); free(feat);
    RET(e);
}

static int c_TECDisposeSniffer(Cpu *c) { RET(((St (*)(uint32_t))SYM(TECDisposeSniffer))(A(0))); }
static int c_TECClearSnifferContextInfo(Cpu *c) { RET(((St (*)(uint32_t))SYM(TECClearSnifferContextInfo))(A(0))); }

#define E(n) { #n, c_##n }
static const struct { const char *name; Stub fn; } g_tec_list[] = {
    E(TECCreateConverter), E(TECCreateConverterFromPath), E(TECDisposeConverter), E(TECClearConverterContextInfo),
    E(TECConvertText), E(TECFlushText), E(TECSetBasicOptions), E(TECGetTextEncodingInternetName),
    E(TECGetTextEncodingFromInternetName), E(TECCountAvailableTextEncodings), E(TECGetAvailableTextEncodings),
    E(TECCountDirectTextEncodingConversions), E(TECGetDirectTextEncodingConversions),
    E(TECCountDestinationTextEncodings), E(TECGetDestinationTextEncodings), E(TECCreateSniffer),
    E(TECSniffTextEncoding), E(TECDisposeSniffer), E(TECClearSnifferContextInfo), E(TECCountMailTextEncodings),
    E(TECGetMailTextEncodings), E(TECCountWebTextEncodings), E(TECGetWebTextEncodings),
};
#undef E
#define TEC_N (sizeof g_tec_list / sizeof g_tec_list[0])

Stub tec_stub(const char *s, size_t n)
{
    size_t k;
    if (n < 6 || memcmp(s, "TEC", 3)) return 0;
    for (k = 0; k < TEC_N; k++)
        if (strlen(g_tec_list[k].name) == n && !memcmp(g_tec_list[k].name, s, n)) return g_tec_list[k].fn;
    return 0;
}
