/* decoder.c - decode Intel instructions (i386, 32-bit code). The decoder itself is in decoder_core.h. */
#include "decoder_core.h"
#include <stdio.h>

unsigned long g_decoded;             /* counter (ROSETTA_STATS shows it) */

int decode(uint32_t a, Insn *b) { return decode_core(a, b); }

const char *insn_name(const Insn *b)
{
    static char t[32];
    if (b->op < 0x100) snprintf(t, sizeof t, "%02x", b->op);
    else if (b->op < 0x200) snprintf(t, sizeof t, "0f %02x", b->op & 0xff);
    else snprintf(t, sizeof t, "0f %s %02x", b->op < 0x300 ? "38" : "3a", b->op & 0xff);
    return t;
}
