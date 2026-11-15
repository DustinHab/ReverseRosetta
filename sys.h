/* sys.h - system calls of the guest (BSD, Mach, machine-dependent) */
#ifndef SYS_H
#define SYS_H

#include "cpu.h"

void sys_bsd(Cpu *c, int sysenter);     /* int $0x80 or sysenter with eax >= 0 */
void sys_mach(Cpu *c);                  /* int $0x81 */
void sys_machine(Cpu *c);              /* int $0x82 */
void sys_sysenter(Cpu *c);              /* sysenter: arguments from ecx+4, return to edx */
void sys_stats(void);                /* ROSETTA_STATS: time per system call */

/* at startup, before the guest runs: own path (for execve of Intel programs), signal state inherited from the
   parent (blocked signals become the guest mask, ignored ones stay ignored) */
void sys_init(void);

/* ROSETTA_LOG=2: time and thread in front of every log line */
void sys_log_stamp(void);

/* the guest's arguments and program path (KERN_PROCARGS2 and p_comm of the own process) */
void sys_set_args(int argc, char **argv, const char *path);

/* address range of dyld (main.c): system calls from there are the loader's (Intel library redirection) */
extern uint32_t g_dyld_from, g_dyld_to;

/* signals to the guest: pending (from the host handler or kill to itself) and mask of the guest; the dispatcher
   calls sig_deliver when (g_sig_pending & ~g_sig_mask) != 0 */
extern volatile uint32_t g_sig_pending;
extern uint32_t g_sig_mask;
void sig_deliver(Cpu *c);

/* before write access to [a, a + len) (mprotect, vm_protect): make shared-mapped file ranges private */
void sys_make_private(uint32_t a, uint32_t len);

/* [a, a + len) touches the shared library region of the host (0x90000000-0xb0000000): freeing, mapping over or
   changing protection there destroys rosetta itself. Reports with the caller (was) and returns 1 */
int sys_host_range(Cpu *c, uint32_t a, uint32_t len, const char *was);

/* record guest memory calls (mmap, munmap, mprotect, vm_*) for debugging; sys_mem_show prints in the fault
   handler (without stdio) those that touch adr, and the last ones */
void sys_mem_note(Cpu *c, char kind, uint32_t a, uint32_t len);
void sys_mem_show(uint32_t addr);

/* synchronous exception of the guest (sig, si_code) at c->eip: 1 = delivered to its handler (pending, the
   dispatcher builds the frame), 0 = reported, program finished; g_end_signal is then the signal rosetta ends with */
int sys_signal(Cpu *c, int sig, int code, const char *reason);
extern int g_end_signal;

/* memory fault of guest code (SIGSEGV/SIGBUS, si_code, fault address; exact = state is exact): 1 = pending for the
   guest's handler, 0 = no handler (or blocked) */
int sys_fault(Cpu *c, int sig, int code, uint32_t addr, int exact);

#endif
