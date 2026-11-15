/* thread.h - threads of the guest (pthread under i386 Leopard: bsdthread_register/create/terminate) */
#ifndef THREAD_H
#define THREAD_H

#include "cpu.h"

extern int g_threads;                    /* running guest threads (1: no lock needed) */
extern volatile int g_thread_waiting;    /* threads waiting for the lock */
extern Cpu *g_thread_cur;                /* Cpu of the thread that holds the lock */

void thread_start(Cpu *main_cpu, int only_interp);  /* the main thread holds the lock from the start */

/* lock around a blocking host call: glock_release releases it (only if there are other threads) and returns the
   own Cpu (otherwise 0), glock_acquire(c) takes it again afterwards */
Cpu *glock_release(void);
void glock_acquire(Cpu *c);
void thread_release(void);             /* release unconditionally (thread ends) */

/* at the dispatcher (lock held): if another thread is waiting and the time slice is over (or c->yield), let it
   go first */
void thread_yield(Cpu *c);

/* system calls 366, 360, 361 (return: 0 or errno; thread_spawn returns the guest pthread_t in *val) */
int thread_register(uint32_t start, uint32_t wqstart, uint32_t pthsize);
int thread_spawn(Cpu *c, uint32_t fn, uint32_t arg, uint32_t stack, uint32_t pthread, uint32_t flags, uint32_t *val);
void thread_finish(Cpu *c, uint32_t stack, uint32_t size, uint32_t port, uint32_t sem);

/* kernel work queues (pthread_workqueue, NSOperationQueue under 10.5): workq_open (367), workq_ops (368) */
int thread_wq_open(void);
int thread_wq_ops(Cpu *c, uint32_t kind, uint32_t ent, uint32_t prio);

/* watchdog: stops a thread that is stuck in a loop of translated code while another thread or a signal is
   waiting (starts with the first thread or signal handler) */
void thread_watchdog_start(void);
int thread_watchdog_stop(void);          /* before execve; returns whether it was running */

/* fork child: reset the lock and thread bookkeeping (only the calling thread survives); watchdog_again: start the
   watchdog again if it was running (not for a child that only execs) */
void thread_after_fork(Cpu *c, int watchdog_again);

/* statistics (ROSETTA_STATS) */
void thread_stats(void);

/* main.c: end of the guest process (exit from a secondary thread): profile, translation cache, statistics, exit */
void main_end(Cpu *c, int r);
void main_profile_save(void);     /* ROSETTA_HPROFILE: before an end by signal */

#endif
