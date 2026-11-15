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

/* 1 while the interpreter executes guest instructions (not during system calls): a memory fault then belongs to the
   guest, with exact eip and registers (main.c memfault, jit_fault_state) */
extern volatile int g_guest_exec;
void interp_fault_landing(Cpu *c);  /* after the jump back from a memory fault: host signal mask, flags */

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
