/* decoder_core.h - the decoder itself, for inlining: decoder.c turns it into decode(), the interpreter inlines it
 * directly into its instruction loop (call and prologue cost as much per instruction there as the decoding).
 *
 * Only 32-bit addressing (prefix 67 leads to "invalid": does not occur in Mac OS X code). Segment prefixes:
 * only fs/gs count (base for thread data), cs/ds/es/ss are flat.
 */
#ifndef DECODER_CORE_H
#define DECODER_CORE_H

#include "decoder.h"
#include "cpu.h"

/* properties of the one-byte opcodes: bit 0 ModRM, bits 4-7 immediate (0 none, 1 ib, 2 iw, 3 iz, 4 special case) */
#define DK_M 0x01
#define DK_IB 0x10
#define DK_IW 0x20
#define DK_IZ 0x30
#define DK_SO 0x40
#define DK_UG 0x80              /* invalid in 32 bit or not supported */

static const uint8_t dk_in[256] = {
#define M DK_M
#define IB DK_IB
#define IW DK_IW
#define IZ DK_IZ
#define SO DK_SO
    /* 00 */ M, M, M, M, IB, IZ, 0, 0, M, M, M, M, IB, IZ, 0, 0,
    /* 10 */ M, M, M, M, IB, IZ, 0, 0, M, M, M, M, IB, IZ, 0, 0,
    /* 20 */ M, M, M, M, IB, IZ, 0, 0, M, M, M, M, IB, IZ, 0, 0,
    /* 30 */ M, M, M, M, IB, IZ, 0, 0, M, M, M, M, IB, IZ, 0, 0,
    /* 40 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 50 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 60 */ 0, 0, M, M, 0, 0, 0, 0, IZ, M | IZ, IB, M | IB, 0, 0, 0, 0,
    /* 70 */ IB, IB, IB, IB, IB, IB, IB, IB, IB, IB, IB, IB, IB, IB, IB, IB,
    /* 80 */ M | IB, M | IZ, M | IB, M | IB, M, M, M, M, M, M, M, M, M, M, M, M,
    /* 90 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, SO, 0, 0, 0, 0, 0,
    /* a0 */ SO, SO, SO, SO, 0, 0, 0, 0, IB, IZ, 0, 0, 0, 0, 0, 0,
    /* b0 */ IB, IB, IB, IB, IB, IB, IB, IB, IZ, IZ, IZ, IZ, IZ, IZ, IZ, IZ,
    /* c0 */ M | IB, M | IB, IW, 0, M, M, M | IB, M | IZ, SO, 0, IW, 0, 0, IB, 0, 0,
    /* d0 */ M, M, M, M, IB, IB, 0, 0, M, M, M, M, M, M, M, M,
    /* e0 */ IB, IB, IB, IB, IB, IB, IB, IB, IZ, IZ, SO, IB, 0, 0, 0, 0,
    /* f0 */ 0, 0, 0, 0, 0, 0, M | SO, M | SO, 0, 0, 0, 0, 0, 0, M, M,
};

/* 0F table (later entries overwrite earlier ones) */
static const uint8_t dk_two[256] = {
    [0 ... 255] = M,
    [0x05 ... 0x09] = 0, [0x0b] = 0, [0x0e] = 0,
    [0x30 ... 0x37] = 0,
    [0x77] = 0,
    [0x80 ... 0x8f] = IZ,
    [0xa0] = 0, [0xa1] = 0, [0xa2] = 0, [0xa8] = 0, [0xa9] = 0, [0xaa] = 0,
    [0xc8 ... 0xcf] = 0,
    [0x70 ... 0x73] = M | IB,
    [0xa4] = M | IB, [0xac] = M | IB, [0xba] = M | IB,
    [0xc2] = M | IB, [0xc4] = M | IB, [0xc5] = M | IB, [0xc6] = M | IB,
    [0x0f] = DK_UG,             /* 3DNow! */
    [0xff] = DK_UG,
#undef M
#undef IB
#undef IW
#undef IZ
#undef SO
};

/* ModRM and SIB (32-bit addressing) */
static inline __attribute__((always_inline)) void dk_modrm(uint32_t *p, Insn *b)
{
    uint8_t m = (uint8_t)ld8((*p)++);
    b->has_modrm = 1;
    b->mod = m >> 6;
    b->reg = (m >> 3) & 7;
    b->rm = m & 7;
    if (b->mod == 3) return;            /* mem, scale, disp are already 0, basis/index -1 */
    b->mem = 1;
    if (b->rm == 4) {
        uint8_t s = (uint8_t)ld8((*p)++);
        b->scale = s >> 6;
        if (((s >> 3) & 7) != 4) b->index = (s >> 3) & 7;
        if ((s & 7) == 5 && b->mod == 0) { b->disp = ld32(*p); *p += 4; }
        else b->base = s & 7;
    } else if (b->rm == 5 && b->mod == 0) {
        b->disp = ld32(*p); *p += 4;
        return;
    } else
        b->base = b->rm;
    if (b->mod == 1) { b->disp = (uint32_t)(int32_t)(int8_t)ld8(*p); *p += 1; }
    else if (b->mod == 2) { b->disp = ld32(*p); *p += 4; }
}

static inline __attribute__((always_inline)) uint32_t dk_direct(uint32_t *p, int kind, int opsz)
{
    uint32_t v = 0;
    switch (kind) {
    case DK_IB: v = (uint32_t)(int32_t)(int8_t)ld8(*p); *p += 1; break;
    case DK_IW: v = ld16(*p); *p += 2; break;
    case DK_IZ:
        if (opsz == 2) { v = (uint32_t)(int32_t)(int16_t)ld16(*p); *p += 2; }
        else { v = ld32(*p); *p += 4; }
        break;
    }
    return v;
}

/* clear the instruction word by word: memset and structure assignment called the commpage at 40 bytes */
typedef uint32_t __attribute__((__may_alias__)) DkWord;
typedef char dk_insn_is_40_bytes[sizeof(Insn) == 40 ? 1 : -1];

/* decode the instruction at address a; returns the length (0 = invalid, then b->invalid is set) */
static inline __attribute__((always_inline)) int decode_core(uint32_t a, Insn *b)
{
    uint32_t p = a;
    uint8_t o, kind;
    int prefixes = 0;
    DkWord *w = (DkWord *)b;
    g_decoded++;
    w[0] = w[1] = w[2] = w[3] = w[4] = w[5] = w[6] = w[7] = w[8] = w[9] = 0;
    b->eip = a;
    b->opsz = 4;
    b->base = b->index = -1;
    for (;;) {
        o = (uint8_t)ld8(p);
        if (o == 0x66) { b->opsz = 2; b->p66 = 1; }
        else if (o == 0xf2 || o == 0xf3) b->rep = o;
        else if (o == 0xf0) b->lock = 1;
        else if (o == 0x64 || o == 0x65) b->seg = o;
        else if (o == 0x26 || o == 0x2e || o == 0x36 || o == 0x3e) { /* flat */ }
        else if (o == 0x67) { b->invalid = 1; return 0; }
        else break;
        p++;
        if (++prefixes > 14) { b->invalid = 1; return 0; }
    }
    p++;
    if (o != 0x0f) {
        b->op = o;
        kind = dk_in[o];
        if (kind & DK_M) dk_modrm(&p, b);
        switch (o) {
        case 0x9a: case 0xea:               /* ptr16:32 */
            b->imm = ld32(p); b->imm2 = ld16(p + 4); p += 6; break;
        case 0xa0: case 0xa1: case 0xa2: case 0xa3:
            b->disp = ld32(p); p += 4; b->mem = 1; break;
        case 0xc8:                          /* enter iw, ib */
            b->imm = ld16(p); b->imm2 = ld8(p + 2); p += 3; break;
        case 0xf6:
            if (b->reg <= 1) { b->imm = ld8(p); p += 1; }
            break;
        case 0xf7:
            if (b->reg <= 1) b->imm = dk_direct(&p, DK_IZ, b->opsz);
            break;
        default:
            if (kind & 0x70) b->imm = dk_direct(&p, kind & 0x70, b->opsz);
        }
        if (kind & DK_UG) b->invalid = 1;
    } else {
        o = (uint8_t)ld8(p++);
        if (o == 0x38 || o == 0x3a) {
            uint8_t o3 = (uint8_t)ld8(p++);
            b->op = (uint16_t)((o == 0x38 ? OP_0F38 : OP_0F3A) | o3);
            dk_modrm(&p, b);
            if (o == 0x3a) { b->imm = ld8(p); p += 1; }
        } else {
            b->op = (uint16_t)(OP_0F | o);
            kind = dk_two[o];
            if (kind & DK_M) dk_modrm(&p, b);
            if (kind & 0x70) b->imm = dk_direct(&p, kind & 0x70, b->opsz);
            if (kind & DK_UG) b->invalid = 1;
        }
    }
    b->len = (uint8_t)(p - a);
    if (b->len > 15) b->invalid = 1;
    return b->invalid ? 0 : b->len;
}

#endif
