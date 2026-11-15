/* ppc.h - encode PowerPC instructions (32 bit, for the translator) */
#ifndef PPC_H
#define PPC_H

#include <stdint.h>

#define D_(op, t, a, i) ((uint32_t)(op) << 26 | (uint32_t)(t) << 21 | (uint32_t)(a) << 16 | ((uint32_t)(i) & 0xffff))
#define X_(t, a, b, xo, rc) ((uint32_t)31 << 26 | (uint32_t)(t) << 21 | (uint32_t)(a) << 16 | (uint32_t)(b) << 11 | (uint32_t)(xo) << 1 | (rc))

/* arithmetic */
#define ADDI(t, a, i)   D_(14, t, a, i)
#define LI(t, i)        D_(14, t, 0, i)
#define ADDIS(t, a, i)  D_(15, t, a, i)
#define LIS(t, i)       D_(15, t, 0, i)
#define ORI(a, s, u)    D_(24, s, a, u)
#define ORIS(a, s, u)   D_(25, s, a, u)
#define XORI(a, s, u)   D_(26, s, a, u)
#define XORIS(a, s, u)  D_(27, s, a, u)
#define ANDI_(a, s, u)  D_(28, s, a, u)          /* andi. (sets CR0) */
#define ANDIS_(a, s, u) D_(29, s, a, u)
#define ADDIC(t, a, i)  D_(12, t, a, i)          /* addic (sets XER[CA]) */
#define ADDIC_(t, a, i) D_(13, t, a, i)          /* addic. (sets CR0) */
#define DIVW(t, a, b)   X_(t, a, b, 491, 0)
#define DIVWU(t, a, b)  X_(t, a, b, 459, 0)
#define MULLI(t, a, i)  D_(7, t, a, i)
#define SUBFIC(t, a, i) D_(8, t, a, i)
#define ADD(t, a, b)    X_(t, a, b, 266, 0)
#define SUBF(t, a, b)   X_(t, a, b, 40, 0)       /* t = b - a */
#define SUBFC(t, a, b)  X_(t, a, b, 8, 0)
#define SUBFE(t, a, b)  X_(t, a, b, 136, 0)
#define ADDC(t, a, b)   X_(t, a, b, 10, 0)
#define ADDE(t, a, b)   X_(t, a, b, 138, 0)
#define ADDZE(t, a)     X_(t, a, 0, 202, 0)
#define NEG(t, a)       X_(t, a, 0, 104, 0)
#define MULLW(t, a, b)  X_(t, a, b, 235, 0)
#define MULHW(t, a, b)  X_(t, a, b, 75, 0)
#define MULHWU(t, a, b) X_(t, a, b, 11, 0)
#define AND(a, s, b)    X_(s, a, b, 28, 0)
#define ANDC(a, s, b)   X_(s, a, b, 60, 0)
#define OR(a, s, b)     X_(s, a, b, 444, 0)
#define XOR(a, s, b)    X_(s, a, b, 316, 0)
#define NOR(a, s, b)    X_(s, a, b, 124, 0)
#define MR(a, s)        OR(a, s, s)
#define NOP             ORI(0, 0, 0)
#define SLW(a, s, b)    X_(s, a, b, 24, 0)
#define SRW(a, s, b)    X_(s, a, b, 536, 0)
#define SRAW(a, s, b)   X_(s, a, b, 792, 0)
#define SRAWI(a, s, n)  X_(s, a, n, 824, 0)
#define CNTLZW(a, s)    X_(s, a, 0, 26, 0)
#define EXTSB(a, s)     X_(s, a, 0, 954, 0)
#define EXTSH(a, s)     X_(s, a, 0, 922, 0)
#define RLWINM(a, s, sh, mb, me) ((uint32_t)21 << 26 | (uint32_t)(s) << 21 | (uint32_t)(a) << 16 | (uint32_t)(sh) << 11 | (uint32_t)(mb) << 6 | (uint32_t)(me) << 1)
#define RLWIMI(a, s, sh, mb, me) ((uint32_t)20 << 26 | (uint32_t)(s) << 21 | (uint32_t)(a) << 16 | (uint32_t)(sh) << 11 | (uint32_t)(mb) << 6 | (uint32_t)(me) << 1)
#define RLWNM(a, s, b, mb, me) ((uint32_t)23 << 26 | (uint32_t)(s) << 21 | (uint32_t)(a) << 16 | (uint32_t)(b) << 11 | (uint32_t)(mb) << 6 | (uint32_t)(me) << 1)
#define RC_(w)          ((w) | 1)                /* X-form that sets CR0 (and., subf., extsb. ...) */
#define SLWI(a, s, n)   RLWINM(a, s, n, 0, 31 - (n))
#define SRWI(a, s, n)   RLWINM(a, s, 32 - (n), n, 31)

/* compares (cr = field 0..7) */
#define CMPW(cr, a, b)  X_((cr) << 2, a, b, 0, 0)
#define CMPLW(cr, a, b) X_((cr) << 2, a, b, 32, 0)
#define CMPWI(cr, a, i) D_(11, (cr) << 2, a, i)
#define CMPLWI(cr, a, u) D_(10, (cr) << 2, a, u)

/* memory */
#define LWZ(t, d, a)    D_(32, t, a, d)
#define STW(s, d, a)    D_(36, s, a, d)
#define LBZ(t, d, a)    D_(34, t, a, d)
#define STB(s, d, a)    D_(38, s, a, d)
#define LHZ(t, d, a)    D_(40, t, a, d)
#define STH(s, d, a)    D_(44, s, a, d)
#define STWU(s, d, a)   D_(37, s, a, d)
#define LMW(t, d, a)    D_(46, t, a, d)
#define STMW(s, d, a)   D_(47, s, a, d)
#define LWZX(t, a, b)   X_(t, a, b, 23, 0)
#define STWX(s, a, b)   X_(s, a, b, 151, 0)
#define LBZX(t, a, b)   X_(t, a, b, 87, 0)
#define STBX(s, a, b)   X_(s, a, b, 215, 0)
#define LWBRX(t, a, b)  X_(t, a, b, 534, 0)
#define STWBRX(s, a, b) X_(s, a, b, 662, 0)
#define LHBRX(t, a, b)  X_(t, a, b, 790, 0)
#define STHBRX(s, a, b) X_(s, a, b, 918, 0)

/* branches */
#define B_(off)         ((uint32_t)18 << 26 | ((uint32_t)(off) & 0x03fffffc))
#define BL_(off)        (B_(off) | 1)
#define BC_(bo, bi, off) ((uint32_t)16 << 26 | (uint32_t)(bo) << 21 | (uint32_t)(bi) << 16 | ((uint32_t)(off) & 0xfffc))
#define BCTR            ((uint32_t)19 << 26 | 20u << 21 | 528u << 1)
#define BCTRL           (BCTR | 1)
#define BLR             ((uint32_t)19 << 26 | 20u << 21 | 16u << 1)
#define BO_TRUE 12
#define BO_FALSE 4
#define CR_LT 0
#define CR_GT 1
#define CR_EQ 2

/* floating point (the G4 has no fsqrt) */
#define LFD(f, d, a)    D_(50, f, a, d)
#define STFD(f, d, a)   D_(54, f, a, d)
#define LFS(f, d, a)    D_(48, f, a, d)
#define STFS(f, d, a)   D_(52, f, a, d)
#define A_(op, d, a, b, c, xo) ((uint32_t)(op) << 26 | (uint32_t)(d) << 21 | (uint32_t)(a) << 16 | (uint32_t)(b) << 11 | (uint32_t)(c) << 6 | (uint32_t)(xo) << 1)
#define FADD(d, a, b)   A_(63, d, a, b, 0, 21)
#define FSUB(d, a, b)   A_(63, d, a, b, 0, 20)
#define FMUL(d, a, c)   A_(63, d, a, 0, c, 25)
#define FDIV(d, a, b)   A_(63, d, a, b, 0, 18)
#define FADDS(d, a, b)  A_(59, d, a, b, 0, 21)
#define FSUBS(d, a, b)  A_(59, d, a, b, 0, 20)
#define FMULS(d, a, c)  A_(59, d, a, 0, c, 25)
#define FDIVS(d, a, b)  A_(59, d, a, b, 0, 18)
#define FX_(d, b, xo)   ((uint32_t)63 << 26 | (uint32_t)(d) << 21 | (uint32_t)(b) << 11 | (uint32_t)(xo) << 1)
#define FMR(d, b)       FX_(d, b, 72)
#define FNEG(d, b)      FX_(d, b, 40)
#define FABS(d, b)      FX_(d, b, 264)
#define FRSP(d, b)      FX_(d, b, 12)
#define FCTIW(d, b)     FX_(d, b, 14)
#define FCTIWZ(d, b)    FX_(d, b, 15)
#define FCMPU(cr, a, b) ((uint32_t)63 << 26 | (uint32_t)(cr) << 23 | (uint32_t)(a) << 16 | (uint32_t)(b) << 11)
#define CROR(d, a, b)   ((uint32_t)19 << 26 | (uint32_t)(d) << 21 | (uint32_t)(a) << 16 | (uint32_t)(b) << 11 | 449u << 1)
#define CRNOR(d, a, b)  ((uint32_t)19 << 26 | (uint32_t)(d) << 21 | (uint32_t)(a) << 16 | (uint32_t)(b) << 11 | 33u << 1)
#define CRXOR(d, a, b)  ((uint32_t)19 << 26 | (uint32_t)(d) << 21 | (uint32_t)(a) << 16 | (uint32_t)(b) << 11 | 193u << 1)
#define CREQV(d, a, b)  ((uint32_t)19 << 26 | (uint32_t)(d) << 21 | (uint32_t)(a) << 16 | (uint32_t)(b) << 11 | 289u << 1)
#define CR_UN 3                                  /* after fcmpu: unordered (NaN) */

/* AltiVec (content of the XMM registers: raw bytes in guest byte order, element 0 = lowest address) */
#define LVX(v, a, b)    X_(v, a, b, 103, 0)
#define VPERM(d, a, b, c) ((uint32_t)4 << 26 | (uint32_t)(d) << 21 | (uint32_t)(a) << 16 | (uint32_t)(b) << 11 | (uint32_t)(c) << 6 | 43u)
#define STVX(v, a, b)   X_(v, a, b, 231, 0)
#define VX_(xo, d, a, b) ((uint32_t)4 << 26 | (uint32_t)(d) << 21 | (uint32_t)(a) << 16 | (uint32_t)(b) << 11 | (xo))
#define VAND(d, a, b)   VX_(1028, d, a, b)
#define VANDC(d, a, b)  VX_(1092, d, a, b)       /* a & ~b */
#define PRE(d, a, b)    VX_(1156, d, a, b)
#define VXOR(d, a, b)   VX_(1220, d, a, b)
#define VCMPEQUB(d, a, b) VX_(6, d, a, b)
#define VCMPEQUH(d, a, b) VX_(70, d, a, b)
#define VCMPEQUW(d, a, b) VX_(134, d, a, b)
#define VCMPGTSB(d, a, b) VX_(774, d, a, b)
#define VSUM4UBS(d, a, b) VX_(1544, d, a, b)
#define VSPLTISB(d, s)  VX_(780, d, (s) & 31, 0)

/* special-purpose registers */
#define SPR_(n)         ((((uint32_t)(n) & 31) << 5 | (uint32_t)(n) >> 5) << 11)
#define MFSPR(t, n)     ((uint32_t)31 << 26 | (uint32_t)(t) << 21 | SPR_(n) | 339u << 1)
#define MTSPR(n, s)     ((uint32_t)31 << 26 | (uint32_t)(s) << 21 | SPR_(n) | 467u << 1)
#define SPR_VRSAVE 256
#define MFLR(t)         X_(t, 8, 0, 339, 0)
#define MTLR(s)         X_(s, 8, 0, 467, 0)
#define MTCTR(s)        X_(s, 9, 0, 467, 0)
#define MFCTR(d)        X_(d, 9, 0, 339, 0)
#define MFCR(t)         X_(t, 0, 0, 19, 0)
#define MTCRF(s)        ((uint32_t)31 << 26 | (uint32_t)(s) << 21 | 0xffu << 12 | 144u << 1)

#endif
