/* sse.c - SSE, SSE2, SSE3 in the interpreter (and the 0F AE group: fxsave, ldmxcsr, fences).
 *
 * xmm registers are kept as raw Intel bytes (byte 0 = lowest): loads and stores are copies, the element accesses
 * swap the byte order. Mandatory prefixes: none, 66, F3, F2.
 */
#include "interp.h"
#include <stdio.h>
#include <math.h>

void fx_save(Cpu *c, uint32_t a);
void fx_load(Cpu *c, uint32_t a);

#define P_NO 0
#define P_66 1
#define P_F3 2
#define P_F2 3

static inline uint32_t xw(const Xmm *x, int i) { return ld32((uint32_t)(uintptr_t)&x->b[4 * i]); }
static inline void xw_s(Xmm *x, int i, uint32_t v) { st32((uint32_t)(uintptr_t)&x->b[4 * i], v); }
static inline uint32_t xh(const Xmm *x, int i) { return ld16((uint32_t)(uintptr_t)&x->b[2 * i]); }
static inline void xh_s(Xmm *x, int i, uint32_t v) { st16((uint32_t)(uintptr_t)&x->b[2 * i], v); }
static inline uint64_t xq(const Xmm *x, int i) { return ld64((uint32_t)(uintptr_t)&x->b[8 * i]); }
static inline void xq_s(Xmm *x, int i, uint64_t v) { st64((uint32_t)(uintptr_t)&x->b[8 * i], v); }
static inline float xf(const Xmm *x, int i) { union { uint32_t u; float f; } v; v.u = xw(x, i); return v.f; }
static inline void xf_s(Xmm *x, int i, float f) { union { uint32_t u; float f; } v; v.f = f; xw_s(x, i, v.u); }
static inline double xd(const Xmm *x, int i) { union { uint64_t u; double d; } v; v.u = xq(x, i); return v.d; }
static inline void xd_s(Xmm *x, int i, double d) { union { uint64_t u; double d; } v; v.d = d; xq_s(x, i, v.u); }

/* fetch the source operand (r/m); n = bytes from memory (4, 8, 16) */
static Xmm src(Cpu *c, const Insn *b, int n)
{
    Xmm q;
    if (!b->mem) return c->xmm[b->rm];
    memset(&q, 0, sizeof q);
    memcpy(q.b, (const void *)(uintptr_t)op_addr(c, b), (size_t)n);
    return q;
}

static int32_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
static int32_t sat8(int32_t v) { return v > 127 ? 127 : v < -128 ? -128 : v; }
static uint32_t usat8(int32_t v) { return v > 255 ? 255 : v < 0 ? 0 : (uint32_t)v; }
static uint32_t usat16(int32_t v) { return v > 65535 ? 65535 : v < 0 ? 0 : (uint32_t)v; }

static int32_t round_mxcsr(Cpu *c, double v)
{
    double r;
    switch ((c->mxcsr >> 13) & 3) {
    case 0: r = rint(v); break;
    case 1: r = floor(v); break;
    case 2: r = ceil(v); break;
    default: r = trunc(v);
    }
    if (isnan(r) || r >= 2147483648.0 || r < -2147483648.0) return INT32_MIN;
    return (int32_t)r;
}

static int32_t truncate_d(double v)
{
    if (isnan(v) || v >= 2147483648.0 || v < -2147483648.0) return INT32_MIN;
    return (int32_t)trunc(v);
}

static int cmp_pred(int p, double a, double b)
{
    int u = isnan(a) || isnan(b);
    switch (p & 7) {
    case 0: return !u && a == b;
    case 1: return !u && a < b;
    case 2: return !u && a <= b;
    case 3: return u;
    case 4: return u || a != b;
    case 5: return u || !(a < b);
    case 6: return u || !(a <= b);
    default: return !u;
    }
}

static void comi(Cpu *c, double a, double b)
{
    uint32_t f = flags_fetch(c) & ~(F_ZF | F_PF | F_CF | F_OF | F_SF | F_AF);
    if (isnan(a) || isnan(b)) f |= F_ZF | F_PF | F_CF;
    else if (a < b) f |= F_CF;
    else if (a == b) f |= F_ZF;
    flags_set(c, f);
}

static double minmax(int max, double a, double b) { return max ? (a > b ? a : b) : (a < b ? a : b); }

/* packed integer operations on elements */
static void int_binary(Xmm *d, const Xmm *s, int op)
{
    int i;
    Xmm r = *d;
    switch (op) {
    case 0xfc: for (i = 0; i < 16; i++) r.b[i] = (uint8_t)(d->b[i] + s->b[i]); break;
    case 0xfd: for (i = 0; i < 8; i++) xh_s(&r, i, xh(d, i) + xh(s, i)); break;
    case 0xfe: for (i = 0; i < 4; i++) xw_s(&r, i, xw(d, i) + xw(s, i)); break;
    case 0xd4: for (i = 0; i < 2; i++) xq_s(&r, i, xq(d, i) + xq(s, i)); break;
    case 0xf8: for (i = 0; i < 16; i++) r.b[i] = (uint8_t)(d->b[i] - s->b[i]); break;
    case 0xf9: for (i = 0; i < 8; i++) xh_s(&r, i, xh(d, i) - xh(s, i)); break;
    case 0xfa: for (i = 0; i < 4; i++) xw_s(&r, i, xw(d, i) - xw(s, i)); break;
    case 0xfb: for (i = 0; i < 2; i++) xq_s(&r, i, xq(d, i) - xq(s, i)); break;
    case 0xdb: for (i = 0; i < 16; i++) r.b[i] = d->b[i] & s->b[i]; break;
    case 0xdf: for (i = 0; i < 16; i++) r.b[i] = (uint8_t)(~d->b[i] & s->b[i]); break;
    case 0xeb: for (i = 0; i < 16; i++) r.b[i] = d->b[i] | s->b[i]; break;
    case 0xef: for (i = 0; i < 16; i++) r.b[i] = d->b[i] ^ s->b[i]; break;
    case 0xdc: for (i = 0; i < 16; i++) r.b[i] = (uint8_t)usat8(d->b[i] + s->b[i]); break;
    case 0xdd: for (i = 0; i < 8; i++) xh_s(&r, i, usat16((int32_t)(xh(d, i) + xh(s, i)))); break;
    case 0xd8: for (i = 0; i < 16; i++) r.b[i] = (uint8_t)usat8(d->b[i] - s->b[i]); break;
    case 0xd9: for (i = 0; i < 8; i++) xh_s(&r, i, usat16((int32_t)xh(d, i) - (int32_t)xh(s, i))); break;
    case 0xec: for (i = 0; i < 16; i++) r.b[i] = (uint8_t)sat8((int8_t)d->b[i] + (int8_t)s->b[i]); break;
    case 0xed: for (i = 0; i < 8; i++) xh_s(&r, i, (uint32_t)sat16((int16_t)xh(d, i) + (int16_t)xh(s, i))); break;
    case 0xe8: for (i = 0; i < 16; i++) r.b[i] = (uint8_t)sat8((int8_t)d->b[i] - (int8_t)s->b[i]); break;
    case 0xe9: for (i = 0; i < 8; i++) xh_s(&r, i, (uint32_t)sat16((int16_t)xh(d, i) - (int16_t)xh(s, i))); break;
    case 0xda: for (i = 0; i < 16; i++) r.b[i] = d->b[i] < s->b[i] ? d->b[i] : s->b[i]; break;
    case 0xde: for (i = 0; i < 16; i++) r.b[i] = d->b[i] > s->b[i] ? d->b[i] : s->b[i]; break;
    case 0xea: for (i = 0; i < 8; i++) xh_s(&r, i, (int16_t)xh(d, i) < (int16_t)xh(s, i) ? xh(d, i) : xh(s, i)); break;
    case 0xee: for (i = 0; i < 8; i++) xh_s(&r, i, (int16_t)xh(d, i) > (int16_t)xh(s, i) ? xh(d, i) : xh(s, i)); break;
    case 0xe0: for (i = 0; i < 16; i++) r.b[i] = (uint8_t)((d->b[i] + s->b[i] + 1) >> 1); break;
    case 0xe3: for (i = 0; i < 8; i++) xh_s(&r, i, (xh(d, i) + xh(s, i) + 1) >> 1); break;
    case 0xd5: for (i = 0; i < 8; i++) xh_s(&r, i, xh(d, i) * xh(s, i)); break;
    case 0xe5: for (i = 0; i < 8; i++) xh_s(&r, i, (uint32_t)(((int32_t)(int16_t)xh(d, i) * (int16_t)xh(s, i)) >> 16)); break;
    case 0xe4: for (i = 0; i < 8; i++) xh_s(&r, i, (xh(d, i) * xh(s, i)) >> 16); break;
    case 0xf4: for (i = 0; i < 2; i++) xq_s(&r, i, (uint64_t)xw(d, 2 * i) * xw(s, 2 * i)); break;
    case 0xf5:
        for (i = 0; i < 4; i++)
            xw_s(&r, i, (uint32_t)((int32_t)(int16_t)xh(d, 2 * i) * (int16_t)xh(s, 2 * i) +
                                   (int32_t)(int16_t)xh(d, 2 * i + 1) * (int16_t)xh(s, 2 * i + 1)));
        break;
    case 0xf6: {
        int k;
        for (k = 0; k < 2; k++) {
            uint32_t sum = 0;
            for (i = 0; i < 8; i++) { int v = d->b[8 * k + i] - s->b[8 * k + i]; sum += (uint32_t)(v < 0 ? -v : v); }
            xq_s(&r, k, sum);
        }
        break;
    }
    case 0x74: for (i = 0; i < 16; i++) r.b[i] = d->b[i] == s->b[i] ? 0xff : 0; break;
    case 0x75: for (i = 0; i < 8; i++) xh_s(&r, i, xh(d, i) == xh(s, i) ? 0xffff : 0); break;
    case 0x76: for (i = 0; i < 4; i++) xw_s(&r, i, xw(d, i) == xw(s, i) ? 0xffffffff : 0); break;
    case 0x64: for (i = 0; i < 16; i++) r.b[i] = (int8_t)d->b[i] > (int8_t)s->b[i] ? 0xff : 0; break;
    case 0x65: for (i = 0; i < 8; i++) xh_s(&r, i, (int16_t)xh(d, i) > (int16_t)xh(s, i) ? 0xffff : 0); break;
    case 0x66: for (i = 0; i < 4; i++) xw_s(&r, i, (int32_t)xw(d, i) > (int32_t)xw(s, i) ? 0xffffffff : 0); break;
    case 0x60: for (i = 0; i < 8; i++) { r.b[2 * i] = d->b[i]; r.b[2 * i + 1] = s->b[i]; } break;
    case 0x61: for (i = 0; i < 4; i++) { xh_s(&r, 2 * i, xh(d, i)); xh_s(&r, 2 * i + 1, xh(s, i)); } break;
    case 0x62: for (i = 0; i < 2; i++) { xw_s(&r, 2 * i, xw(d, i)); xw_s(&r, 2 * i + 1, xw(s, i)); } break;
    case 0x6c: xq_s(&r, 0, xq(d, 0)); xq_s(&r, 1, xq(s, 0)); break;
    case 0x68: for (i = 0; i < 8; i++) { r.b[2 * i] = d->b[8 + i]; r.b[2 * i + 1] = s->b[8 + i]; } break;
    case 0x69: for (i = 0; i < 4; i++) { xh_s(&r, 2 * i, xh(d, 4 + i)); xh_s(&r, 2 * i + 1, xh(s, 4 + i)); } break;
    case 0x6a: for (i = 0; i < 2; i++) { xw_s(&r, 2 * i, xw(d, 2 + i)); xw_s(&r, 2 * i + 1, xw(s, 2 + i)); } break;
    case 0x6d: xq_s(&r, 0, xq(d, 1)); xq_s(&r, 1, xq(s, 1)); break;
    case 0x63:
        for (i = 0; i < 8; i++) { r.b[i] = (uint8_t)sat8((int16_t)xh(d, i)); r.b[8 + i] = (uint8_t)sat8((int16_t)xh(s, i)); }
        break;
    case 0x67:
        for (i = 0; i < 8; i++) { r.b[i] = (uint8_t)usat8((int16_t)xh(d, i)); r.b[8 + i] = (uint8_t)usat8((int16_t)xh(s, i)); }
        break;
    case 0x6b:
        for (i = 0; i < 4; i++) { xh_s(&r, i, (uint32_t)sat16((int32_t)xw(d, i))); xh_s(&r, 4 + i, (uint32_t)sat16((int32_t)xw(s, i))); }
        break;
    }
    *d = r;
}

/* shift packed integers by n bits; art: 0 left, 1 right logical, 2 right arithmetic; g element size */
static void int_shift(Xmm *d, int kind, int g, uint64_t n)
{
    int i;
    if (g == 2) for (i = 0; i < 8; i++) {
        uint32_t v = xh(d, i);
        if (kind == 0) v = n > 15 ? 0 : v << n;
        else if (kind == 1) v = n > 15 ? 0 : v >> n;
        else v = (uint32_t)((int32_t)(int16_t)v >> (n > 15 ? 15 : n));
        xh_s(d, i, v);
    } else if (g == 4) for (i = 0; i < 4; i++) {
        uint32_t v = xw(d, i);
        if (kind == 0) v = n > 31 ? 0 : v << n;
        else if (kind == 1) v = n > 31 ? 0 : v >> n;
        else v = (uint32_t)((int32_t)v >> (n > 31 ? 31 : n));
        xw_s(d, i, v);
    } else for (i = 0; i < 2; i++) {
        uint64_t v = xq(d, i);
        if (kind == 0) v = n > 63 ? 0 : v << n;
        else v = n > 63 ? 0 : v >> n;
        xq_s(d, i, v);
    }
}

int sse_step(Cpu *c, const Insn *b)
{
    int o = b->op & 0xff, p = b->rep == 0xf3 ? P_F3 : b->rep == 0xf2 ? P_F2 : b->p66 ? P_66 : P_NO, i;
    Xmm *d = &c->xmm[b->reg], s;
    uint32_t a;
    if (b->op >= OP_0F38) return RUN_UNKNOWN;

    /* 0F AE: fxsave, fxrstor, ldmxcsr, stmxcsr, fences */
    if (o == 0xae) {
        if (!b->mem) return RUN_OK;        /* lfence, mfence, sfence */
        a = op_addr(c, b);
        switch (b->reg) {
        case 0: fx_save(c, a); return RUN_OK;
        case 1: fx_load(c, a); return RUN_OK;
        case 2: c->mxcsr = ld32(a); return RUN_OK;
        case 3: st32(a, c->mxcsr); return RUN_OK;
        case 7: return RUN_OK;             /* clflush */
        }
        return RUN_UNKNOWN;
    }
    if (o == 0xc3) {                        /* movnti */
        if (!b->mem) return RUN_UNKNOWN;
        st32(op_addr(c, b), c->r[b->reg]);
        return RUN_OK;
    }
    /* MMX (without prefix) is not supported: integer instructions 60-7F, D0-FF without 66 */
    if (p == P_NO && ((o >= 0x60 && o <= 0x7f && o != 0x77) || o >= 0xd0)) return RUN_UNKNOWN;
    if (o == 0x77) return RUN_OK;          /* emms */

    switch (o) {
    case 0x10: case 0x11: {                 /* movups/movupd/movss/movsd */
        int n = p == P_F3 ? 4 : p == P_F2 ? 8 : 16;
        if (o == 0x10) {
            if (b->mem) { memset(d, 0, sizeof *d); memcpy(d->b, (const void *)(uintptr_t)op_addr(c, b), (size_t)n); }
            else memcpy(d->b, c->xmm[b->rm].b, (size_t)n);
        } else {
            if (b->mem) memcpy((void *)(uintptr_t)op_addr(c, b), d->b, (size_t)n);
            else memcpy(c->xmm[b->rm].b, d->b, (size_t)n);
        }
        return RUN_OK;
    }
    case 0x12:
        if (p == P_F2) { s = src(c, b, 8); xq_s(d, 0, xq(&s, 0)); xq_s(d, 1, xq(&s, 0)); return RUN_OK; }   /* movddup */
        if (p == P_F3) { s = src(c, b, 16); xw_s(d, 1, xw(&s, 0)); xw_s(d, 3, xw(&s, 2)); xw_s(d, 0, xw(&s, 0)); xw_s(d, 2, xw(&s, 2)); return RUN_OK; }
        if (!b->mem && p == P_NO) { xq_s(d, 0, xq(&c->xmm[b->rm], 1)); return RUN_OK; }                  /* movhlps */
        memcpy(d->b, (const void *)(uintptr_t)op_addr(c, b), 8);                                           /* movlps/movlpd */
        return RUN_OK;
    case 0x13:
        memcpy((void *)(uintptr_t)op_addr(c, b), d->b, 8);
        return RUN_OK;
    case 0x16:
        if (p == P_F3) { s = src(c, b, 16); xw_s(d, 0, xw(&s, 1)); xw_s(d, 2, xw(&s, 3)); xw_s(d, 1, xw(&s, 1)); xw_s(d, 3, xw(&s, 3)); return RUN_OK; }
        if (!b->mem && p == P_NO) { xq_s(d, 1, xq(&c->xmm[b->rm], 0)); return RUN_OK; }                  /* movlhps */
        memcpy(d->b + 8, (const void *)(uintptr_t)op_addr(c, b), 8);                                       /* movhps/movhpd */
        return RUN_OK;
    case 0x17:
        memcpy((void *)(uintptr_t)op_addr(c, b), d->b + 8, 8);
        return RUN_OK;
    case 0x14: s = src(c, b, 16);
        if (p == P_66) { xq_s(d, 1, xq(&s, 0)); }
        else { uint32_t d1 = xw(d, 1); xw_s(d, 2, d1); xw_s(d, 1, xw(&s, 0)); xw_s(d, 3, xw(&s, 1)); }
        return RUN_OK;
    case 0x15: s = src(c, b, 16);
        if (p == P_66) { xq_s(d, 0, xq(d, 1)); xq_s(d, 1, xq(&s, 1)); }
        else { uint32_t d2 = xw(d, 2), d3 = xw(d, 3); xw_s(d, 0, d2); xw_s(d, 1, xw(&s, 2)); xw_s(d, 2, d3); xw_s(d, 3, xw(&s, 3)); }
        return RUN_OK;
    case 0x28: case 0x6f:                   /* movaps/movapd, movdqa/movdqu */
        if (o == 0x6f && p == P_NO) return RUN_UNKNOWN;
        *d = src(c, b, 16);
        return RUN_OK;
    case 0x29: case 0x2b: case 0x7f: case 0xe7:
        if (b->mem) memcpy((void *)(uintptr_t)op_addr(c, b), d->b, 16); else c->xmm[b->rm] = *d;
        return RUN_OK;
    case 0x2a:                              /* cvtsi2ss/cvtsi2sd (cvtpi2ps: MMX) */
        if (p == P_F3 || p == P_F2) {
            int32_t v = (int32_t)(b->mem ? ld32(op_addr(c, b)) : c->r[b->rm]);
            if (p == P_F3) xf_s(d, 0, (float)v); else xd_s(d, 0, (double)v);
            return RUN_OK;
        }
        return RUN_UNKNOWN;
    case 0x2c: case 0x2d: {                 /* cvt(t)ss2si / cvt(t)sd2si */
        double v;
        if (p != P_F3 && p != P_F2) return RUN_UNKNOWN;
        s = src(c, b, p == P_F3 ? 4 : 8);
        v = p == P_F3 ? xf(&s, 0) : xd(&s, 0);
        c->r[b->reg] = (uint32_t)(o == 0x2c ? truncate_d(v) : round_mxcsr(c, v));
        return RUN_OK;
    }
    case 0x2e: case 0x2f:                   /* (u)comiss / (u)comisd */
        s = src(c, b, p == P_66 ? 8 : 4);
        if (p == P_66) comi(c, xd(d, 0), xd(&s, 0)); else comi(c, xf(d, 0), xf(&s, 0));
        return RUN_OK;
    case 0x50: {                            /* movmskps/pd */
        uint32_t m = 0;
        s = c->xmm[b->rm];
        if (p == P_66) m = (uint32_t)((xq(&s, 0) >> 63) | ((xq(&s, 1) >> 63) << 1));
        else for (i = 0; i < 4; i++) m |= (xw(&s, i) >> 31) << i;
        c->r[b->reg] = m;
        return RUN_OK;
    }
    case 0x51: case 0x52: case 0x53: case 0x58: case 0x59: case 0x5c: case 0x5d: case 0x5e: case 0x5f: {
        int scalar = p == P_F3 || p == P_F2, dop = p == P_66 || p == P_F2, n = dop ? 2 : 4;
        s = src(c, b, scalar ? (dop ? 8 : 4) : 16);
        if (scalar) n = 1;
        for (i = 0; i < n; i++) {
            double x = dop ? xd(d, i) : xf(d, i), y = dop ? xd(&s, i) : xf(&s, i), r;
            if (o == 0x5d || o == 0x5f) {   /* min/max select an operand: its bits unchanged (also a signaling NaN) */
                if (!(o == 0x5d ? x < y : x > y)) { if (dop) xq_s(d, i, xq(&s, i)); else xw_s(d, i, xw(&s, i)); }
                continue;
            }
            switch (o) {
            case 0x51: r = sqrt(y); break;
            case 0x52: r = 1.0 / sqrt(y); break;
            case 0x53: r = 1.0 / y; break;
            case 0x58: r = x + y; break;
            case 0x59: r = x * y; break;
            case 0x5c: r = x - y; break;
            case 0x5d: r = minmax(0, x, y); break;
            case 0x5e: r = x / y; break;
            default: r = minmax(1, x, y); break;
            }
            /* float operands computed in double and rounded to float give exactly the correctly rounded float result
               for + - * / sqrt */
            if (dop) xd_s(d, i, r); else xf_s(d, i, (float)r);
        }
        return RUN_OK;
    }
    case 0x54: case 0x55: case 0x56: case 0x57:
        s = src(c, b, 16);
        for (i = 0; i < 16; i++) {
            uint8_t x = d->b[i], y = s.b[i];
            d->b[i] = o == 0x54 ? x & y : o == 0x55 ? (uint8_t)(~x & y) : o == 0x56 ? x | y : x ^ y;
        }
        return RUN_OK;
    case 0x5a:
        if (p == P_F3) { s = src(c, b, 4); xd_s(d, 0, xf(&s, 0)); }
        else if (p == P_F2) { s = src(c, b, 8); xf_s(d, 0, (float)xd(&s, 0)); }
        else if (p == P_66) { s = src(c, b, 16); xf_s(d, 0, (float)xd(&s, 0)); xf_s(d, 1, (float)xd(&s, 1)); xq_s(d, 1, 0); }
        else { s = src(c, b, 8); double x0 = xf(&s, 0), x1 = xf(&s, 1); xd_s(d, 0, x0); xd_s(d, 1, x1); }
        return RUN_OK;
    case 0x5b:
        s = src(c, b, 16);
        for (i = 0; i < 4; i++) {
            if (p == P_NO) xf_s(d, i, (float)(int32_t)xw(&s, i));
            else if (p == P_66) xw_s(d, i, (uint32_t)round_mxcsr(c, xf(&s, i)));
            else xw_s(d, i, (uint32_t)truncate_d(xf(&s, i)));
        }
        return RUN_OK;
    case 0xe6:
        if (p == P_F3) { s = src(c, b, 8); double x0 = (int32_t)xw(&s, 0), x1 = (int32_t)xw(&s, 1); xd_s(d, 0, x0); xd_s(d, 1, x1); }
        else if (p == P_66 || p == P_F2) {
            s = src(c, b, 16);
            xw_s(d, 0, (uint32_t)(p == P_66 ? truncate_d(xd(&s, 0)) : round_mxcsr(c, xd(&s, 0))));
            xw_s(d, 1, (uint32_t)(p == P_66 ? truncate_d(xd(&s, 1)) : round_mxcsr(c, xd(&s, 1))));
            xq_s(d, 1, 0);
        } else return RUN_UNKNOWN;
        return RUN_OK;
    case 0x6e:                              /* movd xmm, r/m32 */
        memset(d, 0, sizeof *d);
        xw_s(d, 0, b->mem ? ld32(op_addr(c, b)) : c->r[b->rm]);
        return RUN_OK;
    case 0x7e:
        if (p == P_F3) { s = src(c, b, 8); memset(d, 0, sizeof *d); xq_s(d, 0, xq(&s, 0)); return RUN_OK; }   /* movq */
        if (b->mem) st32(op_addr(c, b), xw(d, 0)); else c->r[b->rm] = xw(d, 0);                                /* movd */
        return RUN_OK;
    case 0xd6:                              /* movq xmm/m64, xmm */
        if (b->mem) memcpy((void *)(uintptr_t)op_addr(c, b), d->b, 8);
        else { Xmm *z = &c->xmm[b->rm]; memset(z, 0, sizeof *z); memcpy(z->b, d->b, 8); }
        return RUN_OK;
    case 0x70: {                            /* pshufd, pshufhw, pshuflw */
        uint32_t k = b->imm;
        Xmm r;
        s = src(c, b, 16);
        r = s;
        if (p == P_66) for (i = 0; i < 4; i++) xw_s(&r, i, xw(&s, (k >> (2 * i)) & 3));
        else if (p == P_F3) for (i = 0; i < 4; i++) xh_s(&r, 4 + i, xh(&s, 4 + ((k >> (2 * i)) & 3)));
        else for (i = 0; i < 4; i++) xh_s(&r, i, xh(&s, (k >> (2 * i)) & 3));
        *d = r;
        return RUN_OK;
    }
    case 0x71: case 0x72: case 0x73: {      /* shift by immediate, destination is r/m */
        Xmm *z = &c->xmm[b->rm];
        uint32_t n = b->imm & 0xff;
        int g = o == 0x71 ? 2 : o == 0x72 ? 4 : 8;
        if (o == 0x73 && (b->reg == 3 || b->reg == 7)) {           /* psrldq / pslldq: bytes */
            Xmm r;
            memset(&r, 0, sizeof r);
            if (n < 16) {
                if (b->reg == 3) memcpy(r.b, z->b + n, 16 - n); else memcpy(r.b + n, z->b, 16 - n);
            }
            *z = r;
            return RUN_OK;
        }
        int_shift(z, b->reg == 6 ? 0 : b->reg == 2 ? 1 : 2, g, n);
        return RUN_OK;
    }
    case 0xd1: case 0xd2: case 0xd3: case 0xe1: case 0xe2: case 0xf1: case 0xf2: case 0xf3:
        s = src(c, b, 16);
        int_shift(d, o >= 0xf1 ? 0 : o >= 0xe1 ? 2 : 1, (o & 3) == 1 ? 2 : (o & 3) == 2 ? 4 : 8, xq(&s, 0));
        return RUN_OK;
    case 0xd7: {                            /* pmovmskb */
        uint32_t m = 0;
        for (i = 0; i < 16; i++) m |= (uint32_t)(c->xmm[b->rm].b[i] >> 7) << i;
        c->r[b->reg] = m;
        return RUN_OK;
    }
    case 0xc2: {                            /* cmpps/pd/ss/sd */
        int scalar = p == P_F3 || p == P_F2, dop = p == P_66 || p == P_F2, n = scalar ? 1 : dop ? 2 : 4;
        s = src(c, b, scalar ? (dop ? 8 : 4) : 16);
        for (i = 0; i < n; i++) {
            int w = dop ? cmp_pred((int)b->imm, xd(d, i), xd(&s, i)) : cmp_pred((int)b->imm, xf(d, i), xf(&s, i));
            if (dop) xq_s(d, i, w ? ~0ULL : 0); else xw_s(d, i, w ? 0xffffffffu : 0);
        }
        return RUN_OK;
    }
    case 0xc4:                              /* pinsrw */
        if (p != P_66) return RUN_UNKNOWN;
        xh_s(d, (int)(b->imm & 7), b->mem ? ld16(op_addr(c, b)) : c->r[b->rm] & 0xffff);
        return RUN_OK;
    case 0xc5:                              /* pextrw */
        if (p != P_66) return RUN_UNKNOWN;
        c->r[b->reg] = xh(&c->xmm[b->rm], (int)(b->imm & 7));
        return RUN_OK;
    case 0xc6: {                            /* shufps / shufpd */
        Xmm r;
        uint32_t k = b->imm;
        s = src(c, b, 16);
        if (p == P_66) { xq_s(&r, 0, xq(d, k & 1)); xq_s(&r, 1, xq(&s, (k >> 1) & 1)); }
        else {
            xw_s(&r, 0, xw(d, k & 3)); xw_s(&r, 1, xw(d, (k >> 2) & 3));
            xw_s(&r, 2, xw(&s, (k >> 4) & 3)); xw_s(&r, 3, xw(&s, (k >> 6) & 3));
        }
        *d = r;
        return RUN_OK;
    }
    case 0x7c: case 0x7d: {                 /* haddps/pd, hsubps/pd */
        Xmm r;
        s = src(c, b, 16);
        if (p == P_F2) {
            float a0 = xf(d, 0), a1 = xf(d, 1), a2 = xf(d, 2), a3 = xf(d, 3), b0 = xf(&s, 0), b1 = xf(&s, 1), b2 = xf(&s, 2), b3 = xf(&s, 3);
            if (o == 0x7c) { xf_s(&r, 0, a0 + a1); xf_s(&r, 1, a2 + a3); xf_s(&r, 2, b0 + b1); xf_s(&r, 3, b2 + b3); }
            else { xf_s(&r, 0, a0 - a1); xf_s(&r, 1, a2 - a3); xf_s(&r, 2, b0 - b1); xf_s(&r, 3, b2 - b3); }
        } else if (p == P_66) {
            double a0 = xd(d, 0), a1 = xd(d, 1), b0 = xd(&s, 0), b1 = xd(&s, 1);
            if (o == 0x7c) { xd_s(&r, 0, a0 + a1); xd_s(&r, 1, b0 + b1); }
            else { xd_s(&r, 0, a0 - a1); xd_s(&r, 1, b0 - b1); }
        } else return RUN_UNKNOWN;
        *d = r;
        return RUN_OK;
    }
    case 0xd0:                              /* addsubps/pd */
        s = src(c, b, 16);
        if (p == P_F2) { for (i = 0; i < 4; i++) xf_s(d, i, (i & 1) ? xf(d, i) + xf(&s, i) : xf(d, i) - xf(&s, i)); }
        else { xd_s(d, 0, xd(d, 0) - xd(&s, 0)); xd_s(d, 1, xd(d, 1) + xd(&s, 1)); }
        return RUN_OK;
    case 0xf0:                              /* lddqu */
        if (p != P_F2) return RUN_UNKNOWN;
        *d = src(c, b, 16);
        return RUN_OK;
    case 0xfc: case 0xfd: case 0xfe: case 0xd4: case 0xf8: case 0xf9: case 0xfa: case 0xfb:
    case 0xdb: case 0xdf: case 0xeb: case 0xef: case 0xdc: case 0xdd: case 0xd8: case 0xd9:
    case 0xec: case 0xed: case 0xe8: case 0xe9: case 0xda: case 0xde: case 0xea: case 0xee:
    case 0xe0: case 0xe3: case 0xd5: case 0xe5: case 0xe4: case 0xf4: case 0xf5: case 0xf6:
    case 0x74: case 0x75: case 0x76: case 0x64: case 0x65: case 0x66:
    case 0x60: case 0x61: case 0x62: case 0x6c: case 0x68: case 0x69: case 0x6a: case 0x6d:
    case 0x63: case 0x67: case 0x6b:
        if (p != P_66) return RUN_UNKNOWN;
        s = src(c, b, 16);
        int_binary(d, &s, o);
        return RUN_OK;
    }
    return RUN_UNKNOWN;
}
