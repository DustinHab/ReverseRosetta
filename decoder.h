/* decoder.h - decode Intel instructions (i386, 32 bit) into a structure. Shared by interpreter and translator. */
#ifndef DECODER_H
#define DECODER_H

#include <stdint.h>

/* opcode spaces: 0x000-0x0ff one-byte, 0x100-0x1ff after 0F, 0x200-0x2ff after 0F 38, 0x300-0x3ff after 0F 3A */
#define OP_0F   0x100
#define OP_0F38 0x200
#define OP_0F3A 0x300

typedef struct {
    uint32_t eip;               /* address of the instruction */
    uint8_t len;
    uint16_t op;
    uint8_t opsz;               /* operand size 2 or 4 (the execution recognizes 8-bit instructions by the opcode) */
    uint8_t rep;                /* 0, 0xf2, 0xf3 (with SSE: mandatory prefix) */
    uint8_t p66;                /* 66 seen (with SSE: mandatory prefix, opsz then stays 4) */
    uint8_t seg;                /* 0 = default, otherwise 0x64 (fs) or 0x65 (gs) */
    uint8_t lock;
    uint8_t has_modrm, mod, reg, rm;
    uint8_t mem;                /* r/m denotes memory */
    int8_t base, index;        /* register or -1 */
    uint8_t scale;              /* 0..3 */
    uint32_t disp;
    uint32_t imm, imm2;
    uint8_t invalid;          /* unknown instruction or invalid in 32 bit */
    uint8_t wm_ok, wm;          /* translator: cached write mask (0 = not determined yet) */
} Insn;

/* decode the instruction at address a; returns the length (0 = invalid, then b->invalid is set) */
int decode(uint32_t a, Insn *b);
extern unsigned long g_decoded;
/* name for output (rough only) */
const char *insn_name(const Insn *b);

#endif
