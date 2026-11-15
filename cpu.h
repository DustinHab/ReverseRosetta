/* cpu.h - state of an Intel processor (i386) and memory access to the guest.
 *
 * Guest address = host address: the guest is in the same address space as the translator. The guest is little
 * endian, the host big endian; every access to more than one byte swaps the byte order (on the G4 via
 * lwbrx/stwbrx, at no extra cost).
 */
#ifndef CPU_H
#define CPU_H

#include <stdint.h>
#include <string.h>

enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };

/* EFLAGS */
#define F_CF 0x0001
#define F_PF 0x0004
#define F_AF 0x0010
#define F_ZF 0x0040
#define F_SF 0x0080
#define F_TF 0x0100
#define F_IF 0x0200
#define F_DF 0x0400
#define F_OF 0x0800
#define F_ARITH (F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF)

/* kind of the last flag-setting operation (lazy flags): the flags are computed only when someone reads them */
enum {
    CC_DONE,                      /* flags are in fl */
    CC_ADD, CC_ADC, CC_SUB, CC_SBB, CC_LOGIC, CC_INC, CC_DEC,
    CC_SHL, CC_SHR, CC_SAR, CC_MUL, CC_IMUL
};

typedef union {
    uint8_t b[16];
    uint16_t h[8];
    uint32_t w[4];
    uint64_t q[2];
    float f[4];
    double d[2];
} Xmm;                              /* content in guest byte order (element 0 = lowest) */

typedef struct Cpu {
    uint32_t r[8];
    uint32_t eip;
    uint32_t fl;                    /* EFLAGS without the arithmetic ones while cc_op != CC_DONE */
    uint32_t cc_op, cc_size;     /* size in bytes: 1, 2, 4 */
    uint32_t cc_res, cc_a, cc_b;    /* result and operands of the last operation */
    uint32_t cc_cin;                /* carry before (ADC/SBB) or CF before (INC/DEC) */
    uint32_t gs_base, fs_base;
    /* x87 */
    double st[8];
    int st_top;
    uint32_t x87_depth;             /* occupied registers from TOP (instead of empty bits; the translator keeps them up to date) */
    uint16_t fpu_ctl, fpu_status, fpu_mark;
    /* SSE (16-byte aligned: the translator accesses it with AltiVec lvx/stvx) */
    Xmm xmm[8] __attribute__((aligned(16)));
    uint8_t sse_tmp[16] __attribute__((aligned(16)));    /* scratch area of the translator */
    uint32_t mxcsr;
    /* runtime */
    uint32_t jit_kind;               /* reason for the last exit from translated code (jit.c) */
    uint32_t shadow;              /* pointer into the return stack of the translator (jit.c) */
    uint32_t spec_miss;             /* block whose target prediction was wrong (jit.c: translate again without prediction) */
    uint32_t budget;                /* iteration budget (CTR) of a loop across C calls (jit.c) */
    uint32_t spec_loc;              /* call site whose target prediction was wrong (learn targets, jit.c) */
    uint32_t var_target;              /* exit stub to a variant: target of the call at Cpu.eip (jit.c) */
    uint64_t insns;               /* executed instructions (interpreter) */
    int finished, end_code;
    /* translator: AltiVec (lvx/vperm/stvx) swaps floating-point values from memory into this slot, lfd reads from
       there (the G4 does not forward stw to lfd: the detour via integer registers costs ~60 cycles, this one ~30) */
    uint8_t fp_store[8][16] __attribute__((aligned(16)));
    uint8_t vswap8[16][16] __attribute__((aligned(16)));    /* swap pattern for double at offset a & 15 (two quadwords) */
    uint8_t vswap4[16][16] __attribute__((aligned(16)));    /* the same for float */
    uint8_t vswapp[16][16] __attribute__((aligned(16)));    /* two doubles (16 bytes), each swapped on its own */
    double fw_shadow[12];         /* store forwarding: host copies of stored values (survive C calls) */
    uint8_t fp_pre[32][16] __attribute__((aligned(16)));    /* hoisted floating-point load (swapped, waits for the lfd) */
    /* host form of xmm n at block boundaries (jit.c): xmm_hf[n] = 'D' (low double in xmm_h[n], bytes 0..7 of xmm[n]
       stale) or 'F' (float in the first 4 bytes of xmm_h[n], bytes 0..3 stale), 0: xmm[n] is valid. Code that
       reads xmm[] in Intel format calls xmm_intel() first. */
    double xmm_h[8];
    uint8_t xmm_hf[8] __attribute__((aligned(8)));
    /* threads (thread.c) */
    uint32_t shadow_base;        /* own return ring buffer (jit.c); a flush resets shadow to it */
    int thread_end;                 /* bsdthread_terminate: only this thread ends (finished is then also set) */
    int yield;                  /* at the dispatcher let other threads go first at once (held spin_lock) */
    void *msg;                /* buffer for mach_msg (per thread: the lock is free during the call) */
    uint32_t msg_size;
    int wq_done;                  /* work queue thread: item done (workq_ops THREAD_RETURN) */
    /* memory faults to the guest's handler (main.c memfault): the way back to the dispatcher of this guest thread */
    void *fault_jmp;              /* jmp_buf of the running dispatcher or interpreter loop (0: none) */
    void *host_thread;            /* pthread that runs this guest thread */
    /* OpenGL (gl.c): current context of this thread on the host and its object in guest memory */
    void *gl_ctx;
    uint32_t gl_proxy;
} Cpu;

/* ---------- guest memory ---------- */

#if defined(__ppc__) || defined(__ppc64__)
static inline uint32_t ld32(uint32_t a)
{
    uint32_t v;
    __asm__ volatile("lwbrx %0,0,%1" : "=r"(v) : "r"(a), "m"(*(const uint32_t *)(uintptr_t)a));
    return v;
}
static inline uint32_t ld16(uint32_t a)
{
    uint32_t v;
    __asm__ volatile("lhbrx %0,0,%1" : "=r"(v) : "r"(a), "m"(*(const uint16_t *)(uintptr_t)a));
    return v;
}
static inline void st32(uint32_t a, uint32_t v)
{
    __asm__ volatile("stwbrx %1,0,%2" : "=m"(*(uint32_t *)(uintptr_t)a) : "r"(v), "r"(a));
}
static inline void st16(uint32_t a, uint32_t v)
{
    __asm__ volatile("sthbrx %1,0,%2" : "=m"(*(uint16_t *)(uintptr_t)a) : "r"(v), "r"(a));
}
static inline uint32_t swap32(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
}
#else
/* host little endian (only for tests on the PC) */
static inline uint32_t ld32(uint32_t a) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
static inline uint32_t ld16(uint32_t a) { uint16_t v; memcpy(&v, (const void *)(uintptr_t)a, 2); return v; }
static inline void st32(uint32_t a, uint32_t v) { memcpy((void *)(uintptr_t)a, &v, 4); }
static inline void st16(uint32_t a, uint32_t v) { uint16_t h = (uint16_t)v; memcpy((void *)(uintptr_t)a, &h, 2); }
static inline uint32_t swap32(uint32_t v) { return v; }
#endif

static inline uint32_t ld8(uint32_t a) { return *(const uint8_t *)(uintptr_t)a; }
static inline void st8(uint32_t a, uint32_t v) { *(uint8_t *)(uintptr_t)a = (uint8_t)v; }

static inline uint64_t ld64(uint32_t a) { return (uint64_t)ld32(a) | (uint64_t)ld32(a + 4) << 32; }
static inline void st64(uint32_t a, uint64_t v) { st32(a, (uint32_t)v); st32(a + 4, (uint32_t)(v >> 32)); }

/* convert xmm registers from host form (translator block boundaries) back to Intel format */
static inline void xmm_intel(Cpu *c)
{
    uint32_t a, w[2];
    int n;
    memcpy(w, c->xmm_hf, 8);
    if (!(w[0] | w[1])) return;
    for (n = 0; n < 8; n++) {
        a = (uint32_t)(uintptr_t)c->xmm[n].b;
        if (c->xmm_hf[n] == 'D') { memcpy(w, &c->xmm_h[n], 8); st32(a, w[1]); st32(a + 4, w[0]); }
        else if (c->xmm_hf[n] == 'F') { memcpy(w, &c->xmm_h[n], 4); st32(a, w[0]); }
        c->xmm_hf[n] = 0;
    }
}

/* flags (interp.c) */
uint32_t flags_fetch(Cpu *c);
void flags_set(Cpu *c, uint32_t fl);

#endif
