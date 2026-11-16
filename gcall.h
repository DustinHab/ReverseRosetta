/* gcall.h - guest functions called from host threads (gcall.c) */
#ifndef GCALL_H
#define GCALL_H
#include "cpu.h"

/* create the guest thread that serves the calls (lock held, by a guest thread); 0 or errno */
int gcall_start(Cpu *c);
/* int $0x90 of that thread (interp.c) */
void gcall_trap(Cpu *c);
/* fn(args[0..n-1]) in the guest, the result (eax) in *result; c: the calling guest thread if it holds the lock
   (it is given away while waiting), else 0. Returns 0, or -1 if the call did not finish in timeout_ms */
int guest_call(Cpu *c, uint32_t fn, const uint32_t *args, int n, uint32_t *result, int timeout_ms);
void gcall_stats(void);

#endif
