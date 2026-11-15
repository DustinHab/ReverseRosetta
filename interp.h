/* interp.h - reference interpreter for i386 */
#ifndef INTERP_H
#define INTERP_H

#include "cpu.h"
#include "decoder.h"

/* RUN_SIGNAL: a synchronous exception (division error, ud2, int3, hlt) went to the guest's handler; eip is unchanged
   (int3: after the instruction) and the signal is pending, the caller continues at the dispatcher, which delivers
   it. step() returns it as it is (translated code leaves the block on any value but RUN_OK). */
enum { RUN_OK = 0, RUN_END, RUN_UNKNOWN, RUN_ERROR, RUN_SIGNAL };

/* execute one instruction */
int step(Cpu *c);
int steps(Cpu *c, int max, int *n, int *jumped, volatile int *halt);

/* run until the end or an error */
int run(Cpu *c);

/* print the state (debugging) */
void state_show(Cpu *c, const char *reason);

extern int g_lane;                  /* 1: print every instruction */

/* x87 and SSE (fpu.c, sse.c): execute an instruction, the caller advances eip */
int sse_x87_step(Cpu *c, const Insn *b);
int x87_step(Cpu *c, const Insn *b);
int sse_step(Cpu *c, const Insn *b);
double x87_round(Cpu *c, double v);       /* rounding according to the x87 control word */

/* operands (interp.c) */
int op_condition(Cpu *c, int k);
uint32_t op_addr(Cpu *c, const Insn *b);
uint32_t op_reg_read(Cpu *c, int r, int sz);
void op_reg_write(Cpu *c, int r, int sz, uint32_t v);

#endif
