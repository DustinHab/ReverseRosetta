/* jit.h - translator i386 -> PowerPC */
#ifndef JIT_H
#define JIT_H

#include "cpu.h"

/* execute with the translator (interpreter for everything it cannot do yet) */
int jit_run(Cpu *c);

/* discard translations (mmap/munmap/mprotect of the guest); takes effect at the next stop in the dispatcher */
void jit_flush_schedule(uint32_t a, uint32_t len);

/* statistics */
void jit_stats(void);

/* if a memory fault hit a prefetching instruction (hoisted array accesses): skip it, return 1.
   ctx = ucontext_t * of the signal handler. */
int jit_prefetch_error(void *ctx);
int jit_probe_fault(void);                       /* a read probe of the translator faulted: jumps back (does not return) */
int jit_fault_state(Cpu *c, void *ctx);          /* memory fault: guest state (1 exact, 0 approximate, -1 not the guest's) */

/* crash dump (cache, registers, return ring buffer) to stderr */
void jit_crashdump(void *ctx);
int jit_pagefault(void);
void jit_blocks_sort(void);              /* profile: block table by host address */
void jit_prog(const char *path);            /* prior knowledge: program (key of the hot block list) */
void jit_end(void);                            /* prior knowledge: write the hot block list */
uint32_t jit_guest_eip(uint32_t host);           /* guest eip of the block for a host address (0: none) */
int jit_mapping_expected(uint32_t a, uint32_t len, int fd, uint32_t off);   /* translation cache: mmap places a known library at its address */
void jit_fd_off(int fd);                        /* descriptor closed/replaced (fstat cache) */

/* threads (thread.c) */
void jit_thread_active(Cpu *c);                   /* this thread now holds the lock */
int jit_thread_run(Cpu *c);                   /* secondary thread: own return ring buffer, dispatcher */
int jit_thread_cont(Cpu *c);                   /* the same thread runs again (work queue: next item) */
void jit_thread_unregister(Cpu *c);                /* thread terminated */

/* interrupt (watchdog in thread.c): catch the running thread at the next block start; 1: done */
int jit_interrupt(void);
extern volatile int g_in_code, g_patched;      /* thread is in translated code; block starts stopped */
extern volatile unsigned g_dispatcher_rounds;    /* dispatcher rounds (progress) */
void jit_new_mapped(uint32_t a, uint32_t len);   /* translation cache: mmap without MAP_FIXED landed on pages with old code */

#endif
