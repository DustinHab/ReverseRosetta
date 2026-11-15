/* interp.c - reference interpreter for i386 (integer instructions; x87 and SSE follow in separate files).
 *
 * Flags are kept lazily: the last flag-setting operation records kind, size, result and operands; they are
 * computed only when an instruction reads the flags (flags_fetch).
 */
#include "interp.h"
#include "thread.h"
#include "decoder_core.h"
#include "sys.h"
#include "commpage.h"
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <signal.h>
#include <pthread.h>
#include <mach/mach_time.h>
#include <mach/mach.h>

/* guest word for the crash display: no signal if the address points to unmapped memory */
static int word_safe(uint32_t a, uint32_t *w)
{
    uint32_t raw;
    vm_size_t n = 0;
    if (vm_read_overwrite(mach_task_self(), a, 4, (vm_address_t)(uintptr_t)&raw, &n) != KERN_SUCCESS || n != 4) return 0;
    *w = swap32(raw);
    return 1;
}

int g_lane;

/* the last instruction addresses, for error messages */
#define HISTORY 64
static uint32_t g_history[HISTORY];
static unsigned g_history_pos;

static const uint32_t MASK_BITS[5] = { 0, 0xff, 0xffff, 0, 0xffffffff };
static const uint32_t VZ[5] = { 0, 0x80, 0x8000, 0, 0x80000000 };

/* ---------- flags ---------- */

static int parity(uint32_t v)
{
    v &= 0xff;
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return !(v & 1);
}

uint32_t flags_fetch(Cpu *c)
{
    uint32_t m, s, e, a, b, cf = 0, of = 0, af = 0, n, bits;
    if (c->cc_op == CC_DONE) return c->fl;
    if (c->cc_op == 99) {                   /* debugging (ROSETTA_POISON): flags of a fused compare were read */
        Insn gb;
        fprintf(stderr, "rosetta: poison flags read at eip %08x op %03x (set at %08x)\n", c->eip,
                decode(c->eip, &gb) ? gb.op : 0, c->cc_res);
        c->cc_op = CC_DONE;
        return c->fl;
    }
    m = MASK_BITS[c->cc_size];
    s = VZ[c->cc_size];
    bits = c->cc_size * 8;
    e = c->cc_res & m; a = c->cc_a & m; b = c->cc_b & m;
    switch (c->cc_op) {
    case CC_ADD: cf = e < a; of = ((a ^ e) & (b ^ e) & s) != 0; af = (a ^ b ^ e) & 0x10; break;
    case CC_ADC: cf = c->cc_cin ? e <= a : e < a; of = ((a ^ e) & (b ^ e) & s) != 0; af = (a ^ b ^ e) & 0x10; break;
    case CC_SUB: cf = a < b; of = ((a ^ b) & (a ^ e) & s) != 0; af = (a ^ b ^ e) & 0x10; break;
    case CC_SBB: cf = c->cc_cin ? a <= b : a < b; of = ((a ^ b) & (a ^ e) & s) != 0; af = (a ^ b ^ e) & 0x10; break;
    case CC_LOGIC: break;
    case CC_INC: cf = c->cc_cin; of = e == s; af = (e & 0xf) == 0; break;
    case CC_DEC: cf = c->cc_cin; of = e == s - 1; af = (e & 0xf) == 0xf; break;
    case CC_SHL:
        n = c->cc_b;
        cf = n <= bits ? (a >> (bits - n)) & 1 : 0;
        of = ((e & s) != 0) ^ cf;
        break;
    case CC_SHR:
        n = c->cc_b;
        cf = n <= bits ? (a >> (n - 1)) & 1 : 0;
        of = (a & s) != 0;
        break;
    case CC_SAR: {
        int32_t v = (int32_t)(a << (32 - bits)) >> (32 - bits);
        n = c->cc_b;
        cf = n <= bits ? (uint32_t)(v >> (n - 1)) & 1 : (v < 0);
        of = 0;
        break;
    }
    case CC_MUL: case CC_IMUL: cf = of = c->cc_b != 0; break;
    }
    return (c->fl & ~F_ARITH) | (cf ? F_CF : 0) | (parity(e) ? F_PF : 0) | (af ? F_AF : 0) |
           (e == 0 ? F_ZF : 0) | (e & s ? F_SF : 0) | (of ? F_OF : 0);
}

void flags_set(Cpu *c, uint32_t fl)
{
    c->fl = fl | 0x2;
    c->cc_op = CC_DONE;
}

static inline void cc(Cpu *c, int op, int sz, uint32_t res, uint32_t a, uint32_t b)
{
    c->cc_op = op; c->cc_size = sz; c->cc_res = res; c->cc_a = a; c->cc_b = b;
}

/* condition 0..15 as with Jcc */
static int condition(Cpu *c, int k)
{
    uint32_t f, r;
    /* fast path for the most common case: compare followed by a jump */
    if (c->cc_op == CC_SUB || c->cc_op == CC_LOGIC) {
        uint32_t m = MASK_BITS[c->cc_size], e = c->cc_res & m, a = c->cc_a & m, b = c->cc_b & m;
        if (c->cc_op == CC_SUB) {
            int32_t sa = (int32_t)(a << (32 - c->cc_size * 8)), sb = (int32_t)(b << (32 - c->cc_size * 8));
            switch (k) {
            case 2: return a < b;   case 3: return a >= b;
            case 4: return e == 0;  case 5: return e != 0;
            case 6: return a <= b;  case 7: return a > b;
            case 12: return sa < sb; case 13: return sa >= sb;
            case 14: return sa <= sb; case 15: return sa > sb;
            }
        } else {
            uint32_t s = VZ[c->cc_size];
            switch (k) {
            case 4: return e == 0; case 5: return e != 0;
            case 8: return (e & s) != 0; case 9: return (e & s) == 0;
            }
        }
    }
    f = flags_fetch(c);
    switch (k >> 1) {
    case 0: r = f & F_OF; break;
    case 1: r = f & F_CF; break;
    case 2: r = f & F_ZF; break;
    case 3: r = f & (F_CF | F_ZF); break;
    case 4: r = f & F_SF; break;
    case 5: r = f & F_PF; break;
    case 6: r = !(f & F_SF) != !(f & F_OF); break;
    default: r = (f & F_ZF) || (!(f & F_SF) != !(f & F_OF)); break;
    }
    return (r != 0) ^ (k & 1);
}

/* ---------- operands ---------- */

static inline uint32_t addr_no_seg(Cpu *c, const Insn *b)
{
    uint32_t a = b->disp;
    if (b->base >= 0) a += c->r[b->base];
    if (b->index >= 0) a += c->r[b->index] << b->scale;
    return a;
}

static inline uint32_t addr(Cpu *c, const Insn *b)
{
    uint32_t a = addr_no_seg(c, b);
    if (b->seg == 0x65) a += c->gs_base;
    else if (b->seg == 0x64) a += c->fs_base;
    return a;
}

static inline uint32_t reg_read(Cpu *c, int r, int sz)
{
    if (sz == 4) return c->r[r];
    if (sz == 2) return c->r[r] & 0xffff;
    return r < 4 ? c->r[r] & 0xff : (c->r[r - 4] >> 8) & 0xff;
}

static inline void reg_write(Cpu *c, int r, int sz, uint32_t v)
{
    if (sz == 4) c->r[r] = v;
    else if (sz == 2) c->r[r] = (c->r[r] & 0xffff0000) | (v & 0xffff);
    else if (r < 4) c->r[r] = (c->r[r] & 0xffffff00) | (v & 0xff);
    else c->r[r - 4] = (c->r[r - 4] & 0xffff00ff) | ((v & 0xff) << 8);
}

static inline uint32_t mem_read(uint32_t a, int sz)
{
    return sz == 4 ? ld32(a) : sz == 2 ? ld16(a) : ld8(a);
}

static inline void mem_write(uint32_t a, int sz, uint32_t v)
{
    if (sz == 4) st32(a, v); else if (sz == 2) st16(a, v); else st8(a, v);
}

/* read r/m; *a gets the address (for read-modify-write) */
static inline uint32_t rm_read(Cpu *c, const Insn *b, int sz, uint32_t *a)
{
    if (!b->mem) return reg_read(c, b->rm, sz);
    *a = addr(c, b);
    return mem_read(*a, sz);
}

static inline void rm_write(Cpu *c, const Insn *b, int sz, uint32_t a, uint32_t v)
{
    if (!b->mem) reg_write(c, b->rm, sz, v);
    else mem_write(a, sz, v);
}

/* store first: if the stack page faults, esp is still unchanged (as on x86; the guest's handler sees it, a retry
   pushes again) */
static inline void push(Cpu *c, uint32_t v) { st32(c->r[ESP] - 4, v); c->r[ESP] -= 4; }
static inline uint32_t pop(Cpu *c) { uint32_t v = ld32(c->r[ESP]); c->r[ESP] += 4; return v; }

/* ---------- ALU ---------- */

static uint32_t alu(Cpu *c, int op, int sz, uint32_t a, uint32_t b)
{
    uint32_t m = MASK_BITS[sz], e, cin;
    a &= m; b &= m;
    switch (op) {
    case 0: e = (a + b) & m; cc(c, CC_ADD, sz, e, a, b); return e;
    case 1: e = a | b; cc(c, CC_LOGIC, sz, e, 0, 0); return e;
    case 2: cin = flags_fetch(c) & F_CF; e = (a + b + cin) & m; cc(c, CC_ADC, sz, e, a, b); c->cc_cin = cin; return e;
    case 3: cin = flags_fetch(c) & F_CF; e = (a - b - cin) & m; cc(c, CC_SBB, sz, e, a, b); c->cc_cin = cin; return e;
    case 4: e = a & b; cc(c, CC_LOGIC, sz, e, 0, 0); return e;
    case 5: case 7: e = (a - b) & m; cc(c, CC_SUB, sz, e, a, b); return e;
    default: e = a ^ b; cc(c, CC_LOGIC, sz, e, 0, 0); return e;
    }
}

static void inc_dec(Cpu *c, int dec, int sz, uint32_t *v)
{
    uint32_t cf = flags_fetch(c) & F_CF;
    *v = (dec ? *v - 1 : *v + 1) & MASK_BITS[sz];
    cc(c, dec ? CC_DEC : CC_INC, sz, *v, 0, 0);
    c->cc_cin = cf;
}

/* group 2: shift and rotate; n already masked with 0x1f */
static uint32_t shift(Cpu *c, int kind, int sz, uint32_t a, uint32_t n)
{
    uint32_t m = MASK_BITS[sz], s = VZ[sz], bits = sz * 8, e, f, cf, i;
    a &= m;
    if (n == 0) return a;
    switch (kind) {
    case 0: case 1: {                       /* rol, ror */
        uint32_t k = n % bits;
        e = kind == 0 ? ((a << k) | (a >> ((bits - k) % bits))) & m : ((a >> k) | (a << ((bits - k) % bits))) & m;
        if (k == 0) e = a;
        f = flags_fetch(c) & ~(F_CF | F_OF);
        if (kind == 0) { cf = e & 1; f |= (cf ? F_CF : 0) | ((((e & s) != 0) ^ cf) ? F_OF : 0); }
        else { cf = (e & s) != 0; f |= (cf ? F_CF : 0) | ((((e & s) != 0) ^ ((e & (s >> 1)) != 0)) ? F_OF : 0); }
        flags_set(c, f);
        return e;
    }
    case 2: case 3: {                       /* rcl, rcr */
        uint32_t k = n % (bits + 1);
        f = flags_fetch(c);
        cf = f & F_CF;
        e = a;
        for (i = 0; i < k; i++) {
            if (kind == 2) { uint32_t fresh = (e & s) != 0; e = ((e << 1) | cf) & m; cf = fresh; }
            else { uint32_t fresh = e & 1; e = (e >> 1) | (cf ? s : 0); cf = fresh; }
        }
        f &= ~(F_CF | F_OF);
        if (cf) f |= F_CF;
        if (kind == 2) { if (((e & s) != 0) ^ (cf != 0)) f |= F_OF; }
        else if (((e & s) != 0) ^ ((e & (s >> 1)) != 0)) f |= F_OF;
        flags_set(c, f);
        return e;
    }
    case 4: case 6:                         /* shl */
        e = n < 32 ? (a << n) & m : 0;
        cc(c, CC_SHL, sz, e, a, n);
        return e;
    case 5:                                 /* shr */
        e = n < 32 ? (a >> n) : 0;
        cc(c, CC_SHR, sz, e, a, n);
        return e;
    default: {                              /* sar */
        int32_t v = (int32_t)(a << (32 - bits)) >> (32 - bits);
        e = (uint32_t)(v >> (n < 32 ? n : 31)) & m;
        cc(c, CC_SAR, sz, e, a, n);
        return e;
    }
    }
}

/* #DE: eip stays at the division (fault) */
static int error_div(Cpu *c)
{
    return sys_signal(c, 8 /* SIGFPE */, 7 /* FPE_INTDIV */, "division by zero or overflow") ? RUN_SIGNAL : RUN_ERROR;
}

/* group 3 */
static int group3(Cpu *c, const Insn *b, int sz)
{
    uint32_t a = 0, v = rm_read(c, b, sz, &a), m = MASK_BITS[sz];
    switch (b->reg) {
    case 0: case 1: alu(c, 4, sz, v, b->imm); break;
    case 2: rm_write(c, b, sz, a, ~v & m); break;
    case 3: { uint32_t e = (0 - v) & m; cc(c, CC_SUB, sz, e, 0, v); rm_write(c, b, sz, a, e); break; }
    case 4: case 5: {                       /* mul, imul */
        if (sz == 1) {
            uint32_t al = c->r[EAX] & 0xff, e;
            int ovf;
            if (b->reg == 4) { e = al * v; ovf = (e >> 8) != 0; }
            else { e = (uint32_t)((int32_t)(int8_t)al * (int32_t)(int8_t)v) & 0xffff; ovf = (int32_t)(int16_t)e != (int32_t)(int8_t)e; }
            reg_write(c, EAX, 2, e);
            cc(c, b->reg == 4 ? CC_MUL : CC_IMUL, 1, e, 0, ovf);
        } else if (sz == 2) {
            uint32_t ax = c->r[EAX] & 0xffff, e;
            int ovf;
            if (b->reg == 4) { e = ax * v; ovf = (e >> 16) != 0; }
            else { e = (uint32_t)((int32_t)(int16_t)ax * (int32_t)(int16_t)v); ovf = (int32_t)e != (int32_t)(int16_t)e; }
            reg_write(c, EAX, 2, e);
            reg_write(c, EDX, 2, e >> 16);
            cc(c, b->reg == 4 ? CC_MUL : CC_IMUL, 2, e, 0, ovf);
        } else {
            uint64_t e;
            int ovf;
            if (b->reg == 4) { e = (uint64_t)c->r[EAX] * v; ovf = (e >> 32) != 0; }
            else { int64_t s = (int64_t)(int32_t)c->r[EAX] * (int32_t)v; e = (uint64_t)s; ovf = s != (int32_t)s; }
            c->r[EAX] = (uint32_t)e;
            c->r[EDX] = (uint32_t)(e >> 32);
            cc(c, b->reg == 4 ? CC_MUL : CC_IMUL, 4, (uint32_t)e, 0, ovf);
        }
        break;
    }
    case 6: case 7: {                       /* div, idiv */
        if (v == 0) return error_div(c);
        if (sz == 1) {
            uint32_t ax = c->r[EAX] & 0xffff, q, r;
            if (b->reg == 6) { q = ax / v; r = ax % v; if (q > 0xff) return error_div(c); }
            else {
                int32_t sq = (int16_t)ax / (int8_t)v, sr = (int16_t)ax % (int8_t)v;
                if (sq > 127 || sq < -128) return error_div(c);
                q = (uint32_t)sq & 0xff; r = (uint32_t)sr & 0xff;
            }
            reg_write(c, EAX, 2, (r << 8) | (q & 0xff));
        } else if (sz == 2) {
            uint32_t n = ((c->r[EDX] & 0xffff) << 16) | (c->r[EAX] & 0xffff), q, r;
            if (b->reg == 6) { q = n / v; r = n % v; if (q > 0xffff) return error_div(c); }
            else {
                int32_t sq = (int32_t)n / (int16_t)v, sr = (int32_t)n % (int16_t)v;
                if (sq > 32767 || sq < -32768) return error_div(c);
                q = (uint32_t)sq; r = (uint32_t)sr;
            }
            reg_write(c, EAX, 2, q);
            reg_write(c, EDX, 2, r);
        } else {
            uint64_t n = (uint64_t)c->r[EDX] << 32 | c->r[EAX];
            if (b->reg == 6) {
                uint64_t q = n / v;
                if (q > 0xffffffffULL) return error_div(c);
                c->r[EAX] = (uint32_t)q; c->r[EDX] = (uint32_t)(n % v);
            } else {
                int64_t sn = (int64_t)n, q;
                if ((int32_t)v == -1 && sn == INT64_MIN) return error_div(c);
                q = sn / (int32_t)v;
                if (q > INT32_MAX || q < INT32_MIN) return error_div(c);
                c->r[EAX] = (uint32_t)q; c->r[EDX] = (uint32_t)(sn % (int32_t)v);
            }
        }
        break;
    }
    }
    return RUN_OK;
}

/* string instructions (movs, cmps, stos, lods, scas) with rep */
static void string_op(Cpu *c, const Insn *b, int sz)
{
    int32_t d = (c->fl & F_DF) ? -sz : sz;
    uint32_t op = b->op & ~1u;
    uint32_t src_base = b->seg == 0x65 ? c->gs_base : b->seg == 0x64 ? c->fs_base : 0;
    for (;;) {
        if (b->rep && c->r[ECX] == 0) break;
        switch (op) {
        case 0xa4: mem_write(c->r[EDI], sz, mem_read(c->r[ESI] + src_base, sz)); c->r[ESI] += d; c->r[EDI] += d; break;
        case 0xa6: alu(c, 7, sz, mem_read(c->r[ESI] + src_base, sz), mem_read(c->r[EDI], sz)); c->r[ESI] += d; c->r[EDI] += d; break;
        case 0xaa: mem_write(c->r[EDI], sz, c->r[EAX]); c->r[EDI] += d; break;
        case 0xac: reg_write(c, EAX, sz, mem_read(c->r[ESI] + src_base, sz)); c->r[ESI] += d; break;
        case 0xae: alu(c, 7, sz, c->r[EAX], mem_read(c->r[EDI], sz)); c->r[EDI] += d; break;
        }
        if (!b->rep) break;
        c->r[ECX]--;
        if (op == 0xa6 || op == 0xae) {
            int zf = (c->cc_res & MASK_BITS[sz]) == 0;
            if (b->rep == 0xf3 && !zf) break;
            if (b->rep == 0xf2 && zf) break;
        }
    }
}

/* CPUID: a Core Duo (Yonah) with SSE, SSE2, SSE3; without SSSE3 */
static void cpuid(Cpu *c)
{
    uint32_t f = c->r[EAX];
    switch (f) {
    case 0: c->r[EAX] = 10; c->r[EBX] = 0x756e6547; c->r[EDX] = 0x49656e69; c->r[ECX] = 0x6c65746e; break;
    case 1:
        c->r[EAX] = 0x000006e8;             /* family 6, model 14 */
        c->r[EBX] = 0x00010800;
        c->r[ECX] = 0x00000001;             /* SSE3 */
        c->r[EDX] = 0x0383fbff & ~(1u << 22); /* FPU TSC CX8 CMOV MMX FXSR SSE SSE2 ... */
        c->r[EDX] |= (1u << 25) | (1u << 26) | (1u << 23) | (1u << 15) | (1u << 8) | (1u << 4) | 1u;
        break;
    case 2: c->r[EAX] = 0x02b3b001; c->r[EBX] = 0x000000f0; c->r[ECX] = 0; c->r[EDX] = 0x2c04307d; break;
    case 4: c->r[EAX] = c->r[EBX] = c->r[ECX] = c->r[EDX] = 0; break;
    default: c->r[EAX] = c->r[EBX] = c->r[ECX] = c->r[EDX] = 0;
    }
}

static uint64_t timecounter(void)
{
    /* rdtsc: time base of the G4, scaled up to ~1 GHz */
    static mach_timebase_info_data_t tb;
    uint64_t t = mach_absolute_time();
    if (!tb.denom) mach_timebase_info(&tb);
    return t * tb.numer / tb.denom;
}

/* ---------- execution ---------- */

void state_show(Cpu *c, const char *reason)
{
    Insn b;
    uint32_t f = flags_fetch(c);
    int i;
    fprintf(stderr, "rosetta: %s at eip %08x\n", reason, c->eip);
    fprintf(stderr, "  eax %08x ecx %08x edx %08x ebx %08x\n  esp %08x ebp %08x esi %08x edi %08x\n  fl %08x, %llu instructions\n",
            c->r[EAX], c->r[ECX], c->r[EDX], c->r[EBX], c->r[ESP], c->r[EBP], c->r[ESI], c->r[EDI], f,
            (unsigned long long)c->insns);
    fprintf(stderr, "  last:");
    for (i = 16; i >= 1; i--) fprintf(stderr, " %08x", g_history[(g_history_pos - (unsigned)i) % HISTORY]);
    fprintf(stderr, "\n");
    if (c->eip && (decode(c->eip, &b) || b.invalid)) {
        fprintf(stderr, "  instruction %s, bytes", insn_name(&b));
        for (i = 0; i < 8; i++) fprintf(stderr, " %02x", ld8(c->eip + i));
        fprintf(stderr, "\n");
    }
    {
        uint32_t bp = c->r[EBP], w, next;
        fprintf(stderr, "  stack:");
        for (i = 0; i < 12 && word_safe(c->r[ESP] + 4 * (uint32_t)i, &w); i++) fprintf(stderr, " %08x", w);
        fprintf(stderr, "\n  chain (return, arguments):");
        for (i = 0; i < 12 && !(bp & 3) && word_safe(bp + 4, &w); i++) {
            uint32_t a0 = 0, a1 = 0, a2 = 0;
            word_safe(bp + 8, &a0); word_safe(bp + 12, &a1); word_safe(bp + 16, &a2);
            fprintf(stderr, "\n    %08x (%08x %08x %08x)", w, a0, a1, a2);
            if (!word_safe(bp, &next) || next <= bp) break;
            bp = next;
        }
        fprintf(stderr, "\n");
    }
}

/* One instruction. Inlined into step() and into the loop of steps(): prologue and epilogue of this large function
   (twelve registers, cr3/cr4) cost one sixth of the interpretation time per instruction */
static inline __attribute__((always_inline)) int step_core(Cpu *c)
{
    Insn b;
    uint32_t a = 0, v, w, sz, following;
    int op;
    g_history[g_history_pos++ % HISTORY] = c->eip;
    xmm_intel(c);                           /* xmm registers that the translator has stored in host form */
    if (c->eip - COMMPAGE_BASE < COMMPAGE_SIZE) {
        if (commpage_native(c)) { c->insns++; return RUN_OK; }
        /* empty slot: routine missing (otherwise 00 00 = add %al,(%eax) would silently continue) */
        if (!ld8(c->eip) && !ld8(c->eip + 1)) { state_show(c, "commpage routine missing"); return RUN_UNKNOWN; }
    }
    if (!decode_core(c->eip, &b)) { state_show(c, "unknown instruction"); return RUN_UNKNOWN; }
    if (g_lane) fprintf(stderr, "%08x %-8s eax %08x ecx %08x edx %08x ebx %08x esp %08x\n", c->eip, insn_name(&b),
                        c->r[EAX], c->r[ECX], c->r[EDX], c->r[EBX], c->r[ESP]);
    following = c->eip + b.len;
    c->insns++;
    op = b.op;
    sz = b.opsz;

    /* ALU block 00-3f (without prefixes and BCD) */
    if (op < 0x40 && (op & 7) < 6) {
        int alu_op = op >> 3, g8 = !(op & 1);
        int g = g8 ? 1 : sz;
        switch (op & 7) {
        case 0: case 1:                     /* r/m, r */
            v = rm_read(c, &b, g, &a);
            w = alu(c, alu_op, g, v, reg_read(c, b.reg, g));
            if (alu_op != 7) rm_write(c, &b, g, a, w);
            break;
        case 2: case 3:                     /* r, r/m */
            v = rm_read(c, &b, g, &a);
            w = alu(c, alu_op, g, reg_read(c, b.reg, g), v);
            if (alu_op != 7) reg_write(c, b.reg, g, w);
            break;
        case 4: case 5:                     /* al/eax, imm */
            w = alu(c, alu_op, g, reg_read(c, EAX, g), b.imm);
            if (alu_op != 7) reg_write(c, EAX, g, w);
            break;
        }
        c->eip = following;
        return RUN_OK;
    }

    switch (op) {
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f:
        v = reg_read(c, op & 7, sz);
        inc_dec(c, op >= 0x48, sz, &v);
        reg_write(c, op & 7, sz, v);
        break;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        v = c->r[op & 7];
        if (sz == 2) { c->r[ESP] -= 2; st16(c->r[ESP], v); } else push(c, v);
        break;
    case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
        if (sz == 2) { v = ld16(c->r[ESP]); c->r[ESP] += 2; reg_write(c, op & 7, 2, v); }
        else { v = pop(c); c->r[op & 7] = v; }
        break;
    case 0x60: {                            /* pusha */
        uint32_t sp = c->r[ESP];
        push(c, c->r[EAX]); push(c, c->r[ECX]); push(c, c->r[EDX]); push(c, c->r[EBX]);
        push(c, sp); push(c, c->r[EBP]); push(c, c->r[ESI]); push(c, c->r[EDI]);
        break;
    }
    case 0x61:                              /* popa */
        c->r[EDI] = pop(c); c->r[ESI] = pop(c); c->r[EBP] = pop(c); pop(c);
        c->r[EBX] = pop(c); c->r[EDX] = pop(c); c->r[ECX] = pop(c); c->r[EAX] = pop(c);
        break;
    case 0x68: case 0x6a:
        if (sz == 2) { c->r[ESP] -= 2; st16(c->r[ESP], b.imm); } else push(c, b.imm);
        break;
    case 0x69: case 0x6b: case OP_0F | 0xaf: {  /* imul r, r/m(, imm) */
        int64_t p;
        v = rm_read(c, &b, sz, &a);
        w = op == (OP_0F | 0xaf) ? reg_read(c, b.reg, sz) : b.imm;
        if (sz == 4) p = (int64_t)(int32_t)v * (int32_t)w;
        else p = (int64_t)(int16_t)v * (int16_t)w;
        reg_write(c, b.reg, sz, (uint32_t)p);
        cc(c, CC_IMUL, sz, (uint32_t)p, 0, sz == 4 ? p != (int32_t)p : p != (int16_t)p);
        break;
    }
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f:
        if (condition(c, op & 15)) following += b.imm;
        break;
    case OP_0F | 0x80: case OP_0F | 0x81: case OP_0F | 0x82: case OP_0F | 0x83:
    case OP_0F | 0x84: case OP_0F | 0x85: case OP_0F | 0x86: case OP_0F | 0x87:
    case OP_0F | 0x88: case OP_0F | 0x89: case OP_0F | 0x8a: case OP_0F | 0x8b:
    case OP_0F | 0x8c: case OP_0F | 0x8d: case OP_0F | 0x8e: case OP_0F | 0x8f:
        if (condition(c, op & 15)) following += b.imm;
        break;
    case 0x80: case 0x81: case 0x82: case 0x83: {
        int g = (op == 0x80 || op == 0x82) ? 1 : sz;
        v = rm_read(c, &b, g, &a);
        w = alu(c, b.reg, g, v, b.imm);
        if (b.reg != 7) rm_write(c, &b, g, a, w);
        break;
    }
    case 0x84: case 0x85: {
        int g = op == 0x84 ? 1 : sz;
        v = rm_read(c, &b, g, &a);
        alu(c, 4, g, v, reg_read(c, b.reg, g));
        break;
    }
    case 0x86: case 0x87: {
        int g = op == 0x86 ? 1 : sz;
        v = rm_read(c, &b, g, &a);
        w = reg_read(c, b.reg, g);
        rm_write(c, &b, g, a, w);
        reg_write(c, b.reg, g, v);
        break;
    }
    case 0x88: case 0x89: {
        int g = op == 0x88 ? 1 : sz;
        if (b.mem) mem_write(addr(c, &b), g, reg_read(c, b.reg, g));
        else reg_write(c, b.rm, g, reg_read(c, b.reg, g));
        break;
    }
    case 0x8a: case 0x8b: {
        int g = op == 0x8a ? 1 : sz;
        reg_write(c, b.reg, g, rm_read(c, &b, g, &a));
        break;
    }
    case 0x8c:                              /* mov r/m16, sreg: flat model, fixed selectors */
        v = b.reg == 5 ? 0x37 : b.reg == 1 ? 0x17 : 0x1f;
        if (b.mem) st16(addr(c, &b), v); else reg_write(c, b.rm, b.mem ? 2 : sz, v);
        break;
    case 0x8e:                              /* mov sreg, r/m16: ignored (the system call sets the base) */
        break;
    case 0x8d:
        reg_write(c, b.reg, sz, addr_no_seg(c, &b));
        break;
    case 0x8f:                              /* pop r/m: address after incrementing esp */
        v = pop(c);
        if (b.mem) mem_write(addr(c, &b), sz, v); else reg_write(c, b.rm, sz, v);
        break;
    case 0x90:
        if (b.rep && g_threads > 1) c->yield = 1;   /* pause: spin loop, other threads first (thread.c) */
        break;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        v = reg_read(c, op & 7, sz);
        reg_write(c, op & 7, sz, reg_read(c, EAX, sz));
        reg_write(c, EAX, sz, v);
        break;
    case 0x98:
        if (sz == 2) reg_write(c, EAX, 2, (uint32_t)(int32_t)(int8_t)c->r[EAX]);
        else c->r[EAX] = (uint32_t)(int32_t)(int16_t)c->r[EAX];
        break;
    case 0x99:
        if (sz == 2) reg_write(c, EDX, 2, (c->r[EAX] & 0x8000) ? 0xffff : 0);
        else c->r[EDX] = (c->r[EAX] & 0x80000000) ? 0xffffffff : 0;
        break;
    case 0x9b:                              /* fwait */
        break;
    case 0x9c:
        push(c, (flags_fetch(c) & 0x00fcffff) | 0x202);
        break;
    case 0x9d:
        flags_set(c, (pop(c) & (F_ARITH | F_DF | F_TF | 0x40000 | 0x200000)) | F_IF);
        break;
    case 0x9e:
        flags_set(c, (flags_fetch(c) & ~(F_CF | F_PF | F_AF | F_ZF | F_SF)) |
                        ((c->r[EAX] >> 8) & (F_CF | F_PF | F_AF | F_ZF | F_SF)));
        break;
    case 0x9f:
        reg_write(c, 4, 1, (flags_fetch(c) & 0xd5) | 0x02);
        break;
    case 0xa0: case 0xa1: {
        int g = op == 0xa0 ? 1 : sz;
        a = b.disp + (b.seg == 0x65 ? c->gs_base : b.seg == 0x64 ? c->fs_base : 0);
        reg_write(c, EAX, g, mem_read(a, g));
        break;
    }
    case 0xa2: case 0xa3: {
        int g = op == 0xa2 ? 1 : sz;
        a = b.disp + (b.seg == 0x65 ? c->gs_base : b.seg == 0x64 ? c->fs_base : 0);
        mem_write(a, g, reg_read(c, EAX, g));
        break;
    }
    case 0xa4: case 0xa5: case 0xa6: case 0xa7: case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf:
        string_op(c, &b, (op & 1) ? sz : 1);
        break;
    case 0xa8: case 0xa9:
        alu(c, 4, op == 0xa8 ? 1 : sz, c->r[EAX], b.imm);
        break;
    case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
        reg_write(c, op & 7, 1, b.imm);
        break;
    case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
        reg_write(c, op & 7, sz, b.imm);
        break;
    case 0xc0: case 0xc1: case 0xd0: case 0xd1: case 0xd2: case 0xd3: {
        int g = (op & 1) ? sz : 1;
        uint32_t n = op <= 0xc1 ? b.imm : op <= 0xd1 ? 1 : c->r[ECX];
        v = rm_read(c, &b, g, &a);
        w = shift(c, b.reg, g, v, n & 0x1f);
        rm_write(c, &b, g, a, w);
        break;
    }
    case 0xc2:
        following = pop(c);
        c->r[ESP] += b.imm & 0xffff;
        break;
    case 0xc3:
        following = pop(c);
        break;
    case 0xc6: case 0xc7: {
        int g = op == 0xc6 ? 1 : sz;
        if (b.mem) mem_write(addr(c, &b), g, b.imm); else reg_write(c, b.rm, g, b.imm);
        break;
    }
    case 0xc8: {                            /* enter */
        uint32_t frame;
        push(c, c->r[EBP]);
        frame = c->r[ESP];
        if ((b.imm2 & 31) > 0) {
            uint32_t i;
            for (i = 1; i < (b.imm2 & 31); i++) { c->r[EBP] -= 4; push(c, ld32(c->r[EBP])); }
            push(c, frame);
        }
        c->r[EBP] = frame;
        c->r[ESP] -= b.imm & 0xffff;
        break;
    }
    case 0xc9:
        c->r[ESP] = c->r[EBP];
        c->r[EBP] = pop(c);
        break;
    case 0xcc:
        c->eip = following;                 /* trap: eip after the instruction */
        return sys_signal(c, 5 /* SIGTRAP */, 1 /* TRAP_BRKPT */, "int3") ? RUN_SIGNAL : RUN_ERROR;
    case 0xcd: {
        int was = g_guest_exec;
        c->eip = following;                 /* system calls see eip after the instruction */
        g_guest_exec = 0;                   /* a fault in the translator's system call code is not the guest's */
        switch (b.imm & 0xff) {
        case 0x80: sys_bsd(c, 0); break;
        case 0x81: sys_mach(c); break;
        case 0x82: sys_machine(c); break;
        default: g_guest_exec = was; state_show(c, "int with unknown number"); return RUN_UNKNOWN;
        }
        g_guest_exec = was;
        return c->finished ? RUN_END : RUN_OK;
    }
    case 0xe0: case 0xe1: case 0xe2: {      /* loopne, loope, loop */
        int zf;
        c->r[ECX]--;
        zf = (flags_fetch(c) & F_ZF) != 0;
        if (c->r[ECX] != 0 && (op == 0xe2 || (op == 0xe1 && zf) || (op == 0xe0 && !zf))) following += b.imm;
        break;
    }
    case 0xe3:
        if (c->r[ECX] == 0) following += b.imm;
        break;
    case 0xe8:
        push(c, following);
        following += b.imm;
        break;
    case 0xe9: case 0xeb:
        following += b.imm;
        break;
    case 0xf4:
        return sys_signal(c, 4 /* SIGILL */, 3 /* ILL_PRVOPC */, "hlt") ? RUN_SIGNAL : RUN_ERROR;  /* #GP */
    case 0xf5:
        flags_set(c, flags_fetch(c) ^ F_CF);
        break;
    case 0xf6: case 0xf7: {
        int r = group3(c, &b, op == 0xf6 ? 1 : sz);
        if (r != RUN_OK) return r;
        break;
    }
    case 0xf8: flags_set(c, flags_fetch(c) & ~F_CF); break;
    case 0xf9: flags_set(c, flags_fetch(c) | F_CF); break;
    case 0xfc: c->fl &= ~F_DF; break;
    case 0xfd: c->fl |= F_DF; break;
    case 0xfe:
        if (b.reg > 1) { state_show(c, "FE with invalid reg"); return RUN_UNKNOWN; }
        v = rm_read(c, &b, 1, &a);
        inc_dec(c, b.reg, 1, &v);
        rm_write(c, &b, 1, a, v);
        break;
    case 0xff:
        switch (b.reg) {
        case 0: case 1:
            v = rm_read(c, &b, sz, &a);
            inc_dec(c, b.reg, sz, &v);
            rm_write(c, &b, sz, a, v);
            break;
        case 2:
            v = rm_read(c, &b, 4, &a);
            push(c, following);
            following = v;
            break;
        case 4:
            following = rm_read(c, &b, 4, &a);
            break;
        case 6:
            v = rm_read(c, &b, sz, &a);
            if (sz == 2) { c->r[ESP] -= 2; st16(c->r[ESP], v); } else push(c, v);
            break;
        default:
            state_show(c, "FF with far jump");
            return RUN_UNKNOWN;
        }
        break;

    /* ---------- 0F ---------- */
    case OP_0F | 0x0b:
        return sys_signal(c, 4 /* SIGILL */, 1 /* ILL_ILLOPC */, "ud2") ? RUN_SIGNAL : RUN_ERROR;
    case OP_0F | 0x0d: case OP_0F | 0x18: case OP_0F | 0x19: case OP_0F | 0x1a: case OP_0F | 0x1b:
    case OP_0F | 0x1c: case OP_0F | 0x1d: case OP_0F | 0x1e: case OP_0F | 0x1f:
        break;                              /* prefetch, nop r/m */
    case OP_0F | 0x31: {
        uint64_t t = timecounter();
        c->r[EAX] = (uint32_t)t; c->r[EDX] = (uint32_t)(t >> 32);
        break;
    }
    case OP_0F | 0x34: {                    /* sysenter: the return address is in edx, the stack in ecx */
        int was = g_guest_exec;
        c->eip = following;
        g_guest_exec = 0;
        sys_sysenter(c);
        g_guest_exec = was;
        return c->finished ? RUN_END : RUN_OK;
    }
    case OP_0F | 0x40: case OP_0F | 0x41: case OP_0F | 0x42: case OP_0F | 0x43:
    case OP_0F | 0x44: case OP_0F | 0x45: case OP_0F | 0x46: case OP_0F | 0x47:
    case OP_0F | 0x48: case OP_0F | 0x49: case OP_0F | 0x4a: case OP_0F | 0x4b:
    case OP_0F | 0x4c: case OP_0F | 0x4d: case OP_0F | 0x4e: case OP_0F | 0x4f:
        v = rm_read(c, &b, sz, &a);
        if (condition(c, op & 15)) reg_write(c, b.reg, sz, v);
        break;
    case OP_0F | 0x90: case OP_0F | 0x91: case OP_0F | 0x92: case OP_0F | 0x93:
    case OP_0F | 0x94: case OP_0F | 0x95: case OP_0F | 0x96: case OP_0F | 0x97:
    case OP_0F | 0x98: case OP_0F | 0x99: case OP_0F | 0x9a: case OP_0F | 0x9b:
    case OP_0F | 0x9c: case OP_0F | 0x9d: case OP_0F | 0x9e: case OP_0F | 0x9f:
        v = condition(c, op & 15);
        if (b.mem) st8(addr(c, &b), v); else reg_write(c, b.rm, 1, v);
        break;
    case OP_0F | 0xa0: case OP_0F | 0xa8:   /* push fs/gs */
        push(c, op == (OP_0F | 0xa8) ? 0x37 : 0x1f);
        break;
    case OP_0F | 0xa1: case OP_0F | 0xa9:
        pop(c);
        break;
    case OP_0F | 0xa2:
        cpuid(c);
        break;
    case OP_0F | 0xa3: case OP_0F | 0xab: case OP_0F | 0xb3: case OP_0F | 0xbb: case OP_0F | 0xba: {
        int kind = op == (OP_0F | 0xba) ? b.reg - 4 : op == (OP_0F | 0xa3) ? 0 : op == (OP_0F | 0xab) ? 1 : op == (OP_0F | 0xb3) ? 2 : 3;
        uint32_t bit, msk, f;
        if (op == (OP_0F | 0xba)) { bit = b.imm & (sz * 8 - 1); if (b.reg < 4) { state_show(c, "0F BA"); return RUN_UNKNOWN; } }
        else bit = reg_read(c, b.reg, sz);
        if (b.mem) {
            a = addr(c, &b);
            if (op != (OP_0F | 0xba)) {
                int32_t slide = sz == 4 ? (int32_t)bit >> 5 : (int16_t)bit >> 4;
                a += (uint32_t)(slide * sz);
            }
            bit &= sz * 8 - 1;
            v = mem_read(a, sz);
        } else {
            bit &= sz * 8 - 1;
            v = reg_read(c, b.rm, sz);
        }
        msk = 1u << bit;
        f = flags_fetch(c) & ~F_CF;
        if (v & msk) f |= F_CF;
        flags_set(c, f);
        if (kind == 1) v |= msk; else if (kind == 2) v &= ~msk; else if (kind == 3) v ^= msk;
        if (kind) { if (b.mem) mem_write(a, sz, v); else reg_write(c, b.rm, sz, v); }
        break;
    }
    case OP_0F | 0xa4: case OP_0F | 0xa5: case OP_0F | 0xac: case OP_0F | 0xad: {  /* shld, shrd */
        uint32_t n = ((op & 0xff) == 0xa4 || (op & 0xff) == 0xac ? b.imm : c->r[ECX]) & 0x1f, src, bits = sz * 8, e, f;
        v = rm_read(c, &b, sz, &a);
        src = reg_read(c, b.reg, sz);
        if (n == 0) break;
        if (sz == 4) {
            if ((op & 0xff) <= 0xa5) e = (v << n) | (src >> (32 - n));
            else e = (v >> n) | (src << (32 - n));
        } else {                                /* 16 bit, n > 16 is undefined on Intel */
            uint32_t k = n > 16 ? 16 : n;
            if ((op & 0xff) <= 0xa5) e = ((v << k) | (src >> (16 - k))) & 0xffff;
            else e = ((v >> k) | (src << (16 - k))) & 0xffff;
        }
        rm_write(c, &b, sz, a, e);
        cc(c, CC_LOGIC, sz, e, 0, 0);
        f = flags_fetch(c) & ~(F_CF | F_OF);
        if ((op & 0xff) <= 0xa5) { if ((v >> (bits - n)) & 1) f |= F_CF; }
        else if ((v >> (n - 1)) & 1) f |= F_CF;
        if (((e ^ v) & VZ[sz]) != 0) f |= F_OF;
        flags_set(c, f);
        break;
    }
    case OP_0F | 0xb0: case OP_0F | 0xb1: {     /* cmpxchg */
        int g = op == (OP_0F | 0xb0) ? 1 : sz;
        v = rm_read(c, &b, g, &a);
        alu(c, 7, g, reg_read(c, EAX, g), v);
        if ((reg_read(c, EAX, g) & MASK_BITS[g]) == (v & MASK_BITS[g])) rm_write(c, &b, g, a, reg_read(c, b.reg, g));
        else { reg_write(c, EAX, g, v); if (b.mem) mem_write(a, g, v); }
        break;
    }
    case OP_0F | 0xb6: case OP_0F | 0xb7:
        reg_write(c, b.reg, sz, rm_read(c, &b, op == (OP_0F | 0xb6) ? 1 : 2, &a));
        break;
    case OP_0F | 0xbe:
        reg_write(c, b.reg, sz, (uint32_t)(int32_t)(int8_t)rm_read(c, &b, 1, &a));
        break;
    case OP_0F | 0xbf:
        reg_write(c, b.reg, sz, (uint32_t)(int32_t)(int16_t)rm_read(c, &b, 2, &a));
        break;
    case OP_0F | 0xbc: case OP_0F | 0xbd: {     /* bsf, bsr */
        uint32_t f = flags_fetch(c) & ~F_ZF;
        int i;
        v = rm_read(c, &b, sz, &a) & MASK_BITS[sz];
        if (v == 0) f |= F_ZF;
        else if (op == (OP_0F | 0xbc)) { for (i = 0; !(v & (1u << i)); i++) ; reg_write(c, b.reg, sz, (uint32_t)i); }
        else { for (i = sz * 8 - 1; !(v & (1u << i)); i--) ; reg_write(c, b.reg, sz, (uint32_t)i); }
        flags_set(c, f);
        break;
    }
    case OP_0F | 0xc0: case OP_0F | 0xc1: {     /* xadd */
        int g = op == (OP_0F | 0xc0) ? 1 : sz;
        v = rm_read(c, &b, g, &a);
        w = alu(c, 0, g, v, reg_read(c, b.reg, g));
        reg_write(c, b.reg, g, v);
        rm_write(c, &b, g, a, w);
        break;
    }
    case OP_0F | 0xc7: {                        /* cmpxchg8b */
        uint64_t m;
        uint32_t f = flags_fetch(c) & ~F_ZF;
        if (b.reg != 1 || !b.mem) { state_show(c, "0F C7"); return RUN_UNKNOWN; }
        a = addr(c, &b);
        m = ld64(a);
        if (m == ((uint64_t)c->r[EDX] << 32 | c->r[EAX])) { st64(a, (uint64_t)c->r[ECX] << 32 | c->r[EBX]); f |= F_ZF; }
        else { c->r[EAX] = (uint32_t)m; c->r[EDX] = (uint32_t)(m >> 32); }
        flags_set(c, f);
        break;
    }
    case OP_0F | 0xc8: case OP_0F | 0xc9: case OP_0F | 0xca: case OP_0F | 0xcb:
    case OP_0F | 0xcc: case OP_0F | 0xcd: case OP_0F | 0xce: case OP_0F | 0xcf:
        v = c->r[op & 7];
        c->r[op & 7] = (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
        break;
    default: {
        int r = sse_x87_step(c, &b);         /* fpu.c / sse.c */
        if (r == RUN_UNKNOWN) { state_show(c, "instruction not implemented yet"); return r; }
        if (r != RUN_OK) return r;
        break;
    }
    }
    c->eip = following;
    return RUN_OK;
}

volatile int g_guest_exec;

int step(Cpu *c)
{
    int was = g_guest_exec, r;
    g_guest_exec = 1;
    r = step_core(c);
    g_guest_exec = was;
    return r;
}

/* Up to max instructions straight ahead (translator: cold code). Stops after a jump (*jumped = 1), before the
   commpage, as soon as *halt is set (cache is being flushed) or on an error/end; *n = executed instructions */
int steps(Cpu *c, int max, int *n, int *jumped, volatile int *halt)
{
    int k = 0, r = RUN_OK, was = g_guest_exec;
    *jumped = 0;
    g_guest_exec = 1;
    while (k < max) {
        uint32_t e0 = c->eip;
        r = step_core(c);
        k++;
        if (r != RUN_OK || c->finished) break;     /* also RUN_SIGNAL: the dispatcher delivers it first */
        if (c->eip - e0 - 1 >= 15) { *jumped = 1; break; }
        if (*halt || c->eip - COMMPAGE_BASE < COMMPAGE_SIZE) break;
    }
    g_guest_exec = was;
    *n = k;
    return r;
}

/* the guest's handler for a memory fault takes over: main.c jumped back here out of the host signal handler, which
   leaves SIGSEGV/SIGBUS blocked in the host */
void interp_fault_landing(Cpu *c)
{
    sigset_t m;
    (void)c;
    g_guest_exec = 0;
    sigemptyset(&m);
    sigaddset(&m, SIGSEGV);
    sigaddset(&m, SIGBUS);
    sigprocmask(SIG_UNBLOCK, &m, 0);
}

/* for fpu.c, sse.c and jit.c */
int op_condition(Cpu *c, int k) { return condition(c, k); }
uint32_t op_addr(Cpu *c, const Insn *b) { return addr(c, b); }
uint32_t op_reg_read(Cpu *c, int r, int sz) { return reg_read(c, r, sz); }
void op_reg_write(Cpu *c, int r, int sz, uint32_t v) { reg_write(c, r, sz, v); }

/* debugging (interpreter only): ROSETTA_HALT=addr[,addr...] shows registers and stack each time one is reached */
static int run_halt(Cpu *c, const uint32_t *halt, int nh)
{
    int r, k;
    unsigned long hits = 0;
    for (;;) {
        for (k = 0; k < nh; k++)
            if (c->eip == halt[k] && hits++ < 200) {
                uint32_t s = c->r[ESP];
                fprintf(stderr, "HALT %08x: eax %08x ecx %08x edx %08x ebx %08x esi %08x edi %08x ebp %08x esp %08x fl %08x | "
                        "[esp] %08x %08x %08x %08x %08x %08x\n", c->eip, c->r[EAX], c->r[ECX], c->r[EDX], c->r[EBX], c->r[ESI],
                        c->r[EDI], c->r[EBP], s, flags_fetch(c), ld32(s), ld32(s + 4), ld32(s + 8), ld32(s + 12), ld32(s + 16),
                        ld32(s + 20));
                {                           /* call chain via ebp (functions with a stack frame) */
                    uint32_t bp = c->r[EBP];
                    int t;
                    fprintf(stderr, "  chain:");
                    for (t = 0; t < 16 && bp > c->r[ESP] - 64 && bp < 0xc0000000u && !(bp & 3); t++) {
                        fprintf(stderr, " %08x", ld32(bp + 4));
                        if (ld32(bp) <= bp) break;
                        bp = ld32(bp);
                    }
                    fprintf(stderr, "\n");
                }
            }
        g_guest_exec = 1;
        r = step_core(c);
        g_guest_exec = 0;
        if (r != RUN_OK && r != RUN_SIGNAL) return r;
        if (g_sig_pending & ~g_sig_mask) { sig_deliver(c); if (c->finished) return RUN_END; }
    }
}

static int run_loop(Cpu *c)
{
    int r;
    static uint32_t halt[8];
    static int nh = -1;
    if (nh < 0) {
        const char *h = getenv("ROSETTA_HALT");
        nh = 0;
        while (h && *h && nh < 8) { halt[nh++] = (uint32_t)strtoul(h, (char **)&h, 16); if (*h == ',') h++; else break; }
    }
    if (nh > 0) return run_halt(c, halt, nh);
    for (;;) {
        g_guest_exec = 1;
        r = step_core(c);
        g_guest_exec = 0;
        if (r != RUN_OK && r != RUN_SIGNAL) break;
        if (g_thread_waiting | c->yield) thread_yield(c);     /* other guest threads (thread.c) */
        if (g_sig_pending & ~g_sig_mask) {   /* signal to the guest */
            sig_deliver(c);
            if (c->finished) return RUN_END;
        }
    }
    return r;
}

/* interpreter only (ROSETTA_INTERP=1, no space for the translator): a memory fault of the guest comes back here
   (main.c memfault) and goes to the guest's handler before the next instruction */
int run(Cpu *c)
{
    jmp_buf jb;
    void *prev = c->fault_jmp;
    int r;
    if (_setjmp(jb)) {
        interp_fault_landing(c);
        sig_deliver(c);
        if (c->finished) { c->fault_jmp = prev; return RUN_END; }
    }
    c->fault_jmp = (void *)jb;
    c->host_thread = (void *)pthread_self();
    r = run_loop(c);
    c->fault_jmp = prev;
    return r;
}
