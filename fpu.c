/* fpu.c - x87 in the interpreter, dispatcher for x87/SSE.
 *
 * The x87 registers are kept as double (80-bit values are converted on load/store; the extra precision is lost).
 * i386 functions return float/double in st(0); Mac programs mostly compute with SSE.
 */
#include "interp.h"
#include "sys.h"
#include <stdio.h>
#include <math.h>

#define ST(i) c->st[(c->st_top + (i)) & 7]

/* status: C0 0x100, C1 0x200, C2 0x400, C3 0x4000, TOP bits 11-13 */
#define C0 0x0100
#define C1 0x0200
#define C2 0x0400
#define C3 0x4000

/* empty registers: the stack depth instead of a bit mask (the x87_depth registers from TOP are occupied).
   Equivalent for normal stack code; the translator updates it at the block end with one addition. */
static int empty(Cpu *c, int phys) { return ((phys - c->st_top) & 7) >= (int)c->x87_depth; }

static void push(Cpu *c, double v)
{
    c->st_top = (c->st_top - 1) & 7;
    c->st[c->st_top] = v;
    if (c->x87_depth < 8) c->x87_depth++;
}

static void pop(Cpu *c)
{
    c->st_top = (c->st_top + 1) & 7;
    if (c->x87_depth > 0) c->x87_depth--;
}

/* depth from empty bits (bit i = physical register i empty): occupied from TOP up to the first empty one */
static void depth_off_empty(Cpu *c, unsigned emptybits)
{
    int k;
    for (k = 0; k < 8 && !(emptybits & (1u << ((c->st_top + k) & 7))); k++) ;
    c->x87_depth = (uint32_t)k;
}

static uint16_t status(Cpu *c) { return (uint16_t)((c->fpu_status & ~0x3800) | ((c->st_top & 7) << 11)); }

static void cc_set(Cpu *c, uint16_t bits) { c->fpu_status = (uint16_t)((c->fpu_status & ~(C0 | C1 | C2 | C3)) | bits); }

/* ---------- memory formats ---------- */

static float f32(uint32_t a) { union { uint32_t u; float f; } v; v.u = ld32(a); return v.f; }
static double f64(uint32_t a) { union { uint64_t u; double d; } v; v.u = ld64(a); return v.d; }
static void f32_s(uint32_t a, float f) { union { uint32_t u; float f; } v; v.f = f; st32(a, v.u); }
static void f64_s(uint32_t a, double d) { union { uint64_t u; double d; } v; v.d = d; st64(a, v.u); }

static double f80(uint32_t a)
{
    uint64_t m = ld64(a);
    uint32_t se = ld16(a + 8);
    int e = se & 0x7fff;
    double v;
    if (e == 0 && m == 0) v = 0.0;
    else if (e == 0x7fff) v = (m << 1) ? NAN : INFINITY;
    else v = ldexp((double)m, e - 16383 - 63);
    return (se & 0x8000) ? -v : v;
}

static void f80_s(uint32_t a, double d)
{
    uint64_t m = 0;
    uint32_t se = signbit(d) ? 0x8000 : 0;
    if (isnan(d)) { m = 0xc000000000000000ULL; se |= 0x7fff; }
    else if (isinf(d)) { m = 0x8000000000000000ULL; se |= 0x7fff; }
    else if (d != 0.0) {
        int e;
        double f = frexp(fabs(d), &e);      /* d = f * 2^e, f in [0.5, 1) */
        m = (uint64_t)ldexp(f, 64);
        se |= (uint32_t)(e - 1 + 16383) & 0x7fff;
    }
    st64(a, m);
    st16(a + 8, se);
}

/* rounding according to the control word */
static double round_x87(Cpu *c, double v)
{
    switch ((c->fpu_ctl >> 10) & 3) {
    case 0: return rint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

/* for jit.c (fist/fistp with rounding down/up) */
double x87_round(Cpu *c, double v) { return round_x87(c, v); }

static int64_t integer(double v, int bits)
{
    double limit = ldexp(1.0, bits - 1);
    if (isnan(v) || v >= limit || v < -limit) return bits == 64 ? INT64_MIN : (int64_t)(-(int64_t)1 << (bits - 1));
    return (int64_t)v;
}

/* ---------- arithmetic ---------- */

static double calc(int op, double a, double b)
{
    switch (op) {
    case 0: return a + b;
    case 1: return a * b;
    case 4: return a - b;
    case 5: return b - a;
    case 6: return a / b;
    default: return b / a;
    }
}

static void cmp(Cpu *c, double a, double b)
{
    if (isnan(a) || isnan(b)) cc_set(c, C0 | C2 | C3);
    else if (a < b) cc_set(c, C0);
    else if (a == b) cc_set(c, C3);
    else cc_set(c, 0);
}

static void cmp_eflags(Cpu *c, double a, double b)
{
    uint32_t f = flags_fetch(c) & ~(F_ZF | F_PF | F_CF | F_OF | F_SF | F_AF);
    if (isnan(a) || isnan(b)) f |= F_ZF | F_PF | F_CF;
    else if (a < b) f |= F_CF;
    else if (a == b) f |= F_ZF;
    flags_set(c, f);
}

static void fxam(Cpu *c)
{
    double v = ST(0);
    uint16_t k;
    if (!c->x87_depth) k = C3 | C0;
    else if (isnan(v)) k = C0;
    else if (isinf(v)) k = C2 | C0;
    else if (v == 0.0) k = C3;
    else if (fpclassify(v) == FP_SUBNORMAL) k = C3 | C2;
    else k = C2;
    if (signbit(v)) k |= C1;
    cc_set(c, k);
}

static uint16_t marks(Cpu *c)
{
    uint16_t t = 0;
    int i;
    for (i = 0; i < 8; i++) if (empty(c, i)) t |= (uint16_t)(3u << (2 * i));
    return t;
}

static void env_write(Cpu *c, uint32_t a)
{
    st32(a, c->fpu_ctl | 0xffff0000u);
    st32(a + 4, status(c) | 0xffff0000u);
    st32(a + 8, marks(c) | 0xffff0000u);
    st32(a + 12, 0); st32(a + 16, 0); st32(a + 20, 0); st32(a + 24, 0);
}

static void env_read(Cpu *c, uint32_t a)
{
    uint16_t t = (uint16_t)ld32(a + 8);
    int i;
    unsigned l;
    c->fpu_ctl = (uint16_t)ld32(a);
    c->fpu_status = (uint16_t)ld32(a + 4);
    c->st_top = (c->fpu_status >> 11) & 7;
    l = 0;
    for (i = 0; i < 8; i++) if (((t >> (2 * i)) & 3) == 3) l |= 1u << i;
    depth_off_empty(c, l);
}

/* fxsave/fxrstor (512 bytes) */
void fx_save(Cpu *c, uint32_t a)
{
    int i;
    memset((void *)(uintptr_t)a, 0, 512);
    st16(a, c->fpu_ctl);
    st16(a + 2, status(c));
    {
        uint8_t used = 0;
        for (i = 0; i < 8; i++) if (!empty(c, i)) used |= (uint8_t)(1u << i);
        st8(a + 4, used);
    }
    st32(a + 24, c->mxcsr);
    st32(a + 28, 0xffff);
    for (i = 0; i < 8; i++) f80_s(a + 32 + 16 * (uint32_t)i, ST(i));
    for (i = 0; i < 8; i++) memcpy((void *)(uintptr_t)(a + 160 + 16 * (uint32_t)i), c->xmm[i].b, 16);
}

void fx_load(Cpu *c, uint32_t a)
{
    int i;
    c->fpu_ctl = ld16(a);
    c->fpu_status = ld16(a + 2);
    c->st_top = (c->fpu_status >> 11) & 7;
    depth_off_empty(c, (uint8_t)~ld8(a + 4));
    c->mxcsr = ld32(a + 24);
    for (i = 0; i < 8; i++) ST(i) = f80(a + 32 + 16 * (uint32_t)i);
    for (i = 0; i < 8; i++) memcpy(c->xmm[i].b, (const void *)(uintptr_t)(a + 160 + 16 * (uint32_t)i), 16);
}

int x87_step(Cpu *c, const Insn *b)
{
    int op = b->op, r = b->reg, i = b->rm;
    uint32_t a = b->mem ? op_addr(c, b) : 0;
    double t;
    if (b->mem) {
        switch (op) {
        case 0xd8: if (r == 2 || r == 3) { cmp(c, ST(0), f32(a)); if (r == 3) pop(c); } else ST(0) = calc(r, ST(0), f32(a)); return RUN_OK;
        case 0xdc: if (r == 2 || r == 3) { cmp(c, ST(0), f64(a)); if (r == 3) pop(c); } else ST(0) = calc(r, ST(0), f64(a)); return RUN_OK;
        case 0xda: if (r == 2 || r == 3) { cmp(c, ST(0), (int32_t)ld32(a)); if (r == 3) pop(c); } else ST(0) = calc(r, ST(0), (int32_t)ld32(a)); return RUN_OK;
        case 0xde: if (r == 2 || r == 3) { cmp(c, ST(0), (int16_t)ld16(a)); if (r == 3) pop(c); } else ST(0) = calc(r, ST(0), (int16_t)ld16(a)); return RUN_OK;
        case 0xd9:
            switch (r) {
            case 0: push(c, f32(a)); return RUN_OK;
            case 2: f32_s(a, (float)ST(0)); return RUN_OK;
            case 3: f32_s(a, (float)ST(0)); pop(c); return RUN_OK;
            case 4: env_read(c, a); return RUN_OK;
            case 5: c->fpu_ctl = ld16(a); return RUN_OK;
            case 6: env_write(c, a); return RUN_OK;
            case 7: st16(a, c->fpu_ctl); return RUN_OK;
            }
            break;
        case 0xdb:
            switch (r) {
            case 0: push(c, (int32_t)ld32(a)); return RUN_OK;
            case 1: st32(a, (uint32_t)integer(trunc(ST(0)), 32)); pop(c); return RUN_OK;
            case 2: st32(a, (uint32_t)integer(round_x87(c, ST(0)), 32)); return RUN_OK;
            case 3: st32(a, (uint32_t)integer(round_x87(c, ST(0)), 32)); pop(c); return RUN_OK;
            case 5: push(c, f80(a)); return RUN_OK;
            case 7: f80_s(a, ST(0)); pop(c); return RUN_OK;
            }
            break;
        case 0xdd:
            switch (r) {
            case 0: push(c, f64(a)); return RUN_OK;
            case 1: st64(a, (uint64_t)integer(trunc(ST(0)), 64)); pop(c); return RUN_OK;
            case 2: f64_s(a, ST(0)); return RUN_OK;
            case 3: f64_s(a, ST(0)); pop(c); return RUN_OK;
            case 4: {                       /* frstor */
                int k;
                env_read(c, a);
                for (k = 0; k < 8; k++) ST(k) = f80(a + 28 + 10 * (uint32_t)k);
                return RUN_OK;
            }
            case 6: {                       /* fnsave */
                int k;
                env_write(c, a);
                for (k = 0; k < 8; k++) f80_s(a + 28 + 10 * (uint32_t)k, ST(k));
                c->fpu_ctl = 0x037f; c->fpu_status = 0; c->st_top = 0; c->x87_depth = 0;
                return RUN_OK;
            }
            case 7: st16(a, status(c)); return RUN_OK;
            }
            break;
        case 0xdf:
            switch (r) {
            case 0: push(c, (int16_t)ld16(a)); return RUN_OK;
            case 1: st16(a, (uint32_t)integer(trunc(ST(0)), 16)); pop(c); return RUN_OK;
            case 2: st16(a, (uint32_t)integer(round_x87(c, ST(0)), 16)); return RUN_OK;
            case 3: st16(a, (uint32_t)integer(round_x87(c, ST(0)), 16)); pop(c); return RUN_OK;
            case 5: push(c, (double)(int64_t)ld64(a)); return RUN_OK;
            case 7: st64(a, (uint64_t)integer(round_x87(c, ST(0)), 64)); pop(c); return RUN_OK;
            }
            break;
        }
        return RUN_UNKNOWN;
    }
    /* register forms */
    switch (op) {
    case 0xd8:
        if (r == 2 || r == 3) { cmp(c, ST(0), ST(i)); if (r == 3) pop(c); }
        else ST(0) = calc(r, ST(0), ST(i));
        return RUN_OK;
    case 0xdc: case 0xde:                   /* st(i) = st(i) op st(0); sub/div reversed */
        if (op == 0xde && r == 3 && i == 1) { cmp(c, ST(0), ST(1)); pop(c); pop(c); return RUN_OK; }
        if (r == 2 || r == 3) { cmp(c, ST(0), ST(i)); if (r == 3) pop(c); return RUN_OK; }
        {
            int o = r == 4 ? 5 : r == 5 ? 4 : r == 6 ? 7 : r == 7 ? 6 : r;
            ST(i) = calc(o, ST(i), ST(0));
        }
        if (op == 0xde) pop(c);
        return RUN_OK;
    case 0xd9:
        if (r == 0) { t = ST(i); push(c, t); return RUN_OK; }
        if (r == 1) { t = ST(0); ST(0) = ST(i); ST(i) = t; return RUN_OK; }
        switch (b->rm | (r << 3)) {
        case 0x10: return RUN_OK;                                  /* fnop */
        case 0x20: ST(0) = -ST(0); return RUN_OK;
        case 0x21: ST(0) = fabs(ST(0)); return RUN_OK;
        case 0x24: cmp(c, ST(0), 0.0); return RUN_OK;
        case 0x25: fxam(c); return RUN_OK;
        case 0x28: push(c, 1.0); return RUN_OK;
        case 0x29: push(c, M_LN10 / M_LN2); return RUN_OK;
        case 0x2a: push(c, M_LOG2E); return RUN_OK;
        case 0x2b: push(c, M_PI); return RUN_OK;
        case 0x2c: push(c, M_LN2 / M_LN10); return RUN_OK;
        case 0x2d: push(c, M_LN2); return RUN_OK;
        case 0x2e: push(c, 0.0); return RUN_OK;
        case 0x30: ST(0) = exp2(ST(0)) - 1.0; return RUN_OK;
        case 0x31: ST(1) = ST(1) * log2(ST(0)); pop(c); return RUN_OK;
        case 0x32: ST(0) = tan(ST(0)); push(c, 1.0); cc_set(c, 0); return RUN_OK;
        case 0x33: ST(1) = atan2(ST(1), ST(0)); pop(c); return RUN_OK;
        case 0x34: {                                                /* fxtract */
            int e;
            double m = frexp(ST(0), &e);
            ST(0) = (double)(e - 1);
            push(c, m * 2.0);
            return RUN_OK;
        }
        case 0x35: ST(0) = remainder(ST(0), ST(1)); cc_set(c, 0); return RUN_OK;
        case 0x36: c->st_top = (c->st_top - 1) & 7; return RUN_OK;
        case 0x37: c->st_top = (c->st_top + 1) & 7; return RUN_OK;
        case 0x38: ST(0) = fmod(ST(0), ST(1)); cc_set(c, 0); return RUN_OK;
        case 0x39: ST(1) = ST(1) * log2(ST(0) + 1.0); pop(c); return RUN_OK;
        case 0x3a: ST(0) = sqrt(ST(0)); return RUN_OK;
        case 0x3b: t = ST(0); ST(0) = sin(t); push(c, cos(t)); cc_set(c, 0); return RUN_OK;
        case 0x3c: ST(0) = round_x87(c, ST(0)); return RUN_OK;
        case 0x3d: ST(0) = ldexp(ST(0), (int)trunc(ST(1))); return RUN_OK;
        case 0x3e: ST(0) = sin(ST(0)); cc_set(c, 0); return RUN_OK;
        case 0x3f: ST(0) = cos(ST(0)); cc_set(c, 0); return RUN_OK;
        }
        break;
    case 0xda: case 0xdb: {                 /* fcmov, fucomi, fcomi, fninit, fnclex, fucompp */
        uint32_t f = flags_fetch(c);
        int cond;
        if (op == 0xda && r == 5 && i == 1) { cmp(c, ST(0), ST(1)); pop(c); pop(c); return RUN_OK; }
        if (op == 0xdb && r == 4) {
            if (i == 2) { c->fpu_status &= 0x7f00; return RUN_OK; }
            if (i == 3) { c->fpu_ctl = 0x037f; c->fpu_status = 0; c->st_top = 0; c->x87_depth = 0; return RUN_OK; }
            return RUN_OK;
        }
        if (op == 0xdb && (r == 5 || r == 6)) { cmp_eflags(c, ST(0), ST(i)); return RUN_OK; }
        switch (r) {
        case 0: cond = (f & F_CF) != 0; break;
        case 1: cond = (f & F_ZF) != 0; break;
        case 2: cond = (f & (F_CF | F_ZF)) != 0; break;
        default: cond = (f & F_PF) != 0; break;
        }
        if (op == 0xdb) cond = !cond;
        if (r > 3) break;
        if (cond) ST(0) = ST(i);
        return RUN_OK;
    }
    case 0xdd:
        switch (r) {
        case 0: if ((int)c->x87_depth == i + 1) c->x87_depth--; return RUN_OK;       /* ffree (topmost occupied) */
        case 2: ST(i) = ST(0); return RUN_OK;
        case 3: ST(i) = ST(0); pop(c); return RUN_OK;
        case 4: cmp(c, ST(0), ST(i)); return RUN_OK;
        case 5: cmp(c, ST(0), ST(i)); pop(c); return RUN_OK;
        }
        break;
    case 0xdf:
        if (r == 4 && i == 0) { op_reg_write(c, EAX, 2, status(c)); return RUN_OK; }
        if (r == 5 || r == 6) { cmp_eflags(c, ST(0), ST(i)); pop(c); return RUN_OK; }
        break;
    }
    return RUN_UNKNOWN;
}

int sse_x87_step(Cpu *c, const Insn *b)
{
    if (b->op >= 0xd8 && b->op <= 0xdf) return x87_step(c, b);
    if (b->op >= OP_0F) return sse_step(c, b);
    return RUN_UNKNOWN;
}
