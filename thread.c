/* thread.c - threads of the guest.
 *
 * pthread_create of the i386 libSystem calls bsdthread_create (360); the kernel creates stack and pthread structure
 * and starts a thread in the routine that libSystem registered at startup with bsdthread_register (366)
 * (_thread_start), with eax = pthread_t, ebx = Mach port of the thread, ecx = function, edx = argument, edi = stack
 * size, esi = flags, esp = stack end - 16 (xnu bsd/kern/pthread_synch.c). Here every guest thread gets its own host
 * thread with its own Cpu and its own return ring buffer.
 *
 * Guest code (translated or interpreted) only ever runs in one thread at a time: one global lock. On the iBook
 * (one core) this costs no parallelism; in return lock/xchg/cmpxchg and the atomic commpage stubs are atomic by
 * themselves, and translator, map and code cache need no locks of their own. The lock is released only outside
 * translated code: in blocking system calls (sys.c, mach_msg.c; system calls always end a block), at the dispatcher
 * after a time slice when another thread is waiting, and on a held spin_lock.
 * Limit: a pure compute loop without an exit to the dispatcher does not yield (as with asynchronous signals).
 */
#include "thread.h"
#include "jit.h"
#include "interp.h"
#include "sys.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_time.h>

#define PTHREAD_START_CUSTOM 0x01000000u    /* stack and pthread structure from the caller */
#define GUARD 4096u                         /* guard page below the stack (PTH_DEFAULT_GUARDSIZE) */
#define SLICE_US 2000                     /* time slice after which a waiting thread gets its turn */

int g_threads = 1;
volatile int g_thread_waiting;
Cpu *g_thread_cur;

static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_taken = 1;                   /* the main thread holds the lock from the start */
static unsigned g_switch;                  /* how often the lock was taken */
static uint64_t g_since, g_slice;
static uint32_t g_start, g_wqstart, g_pthsize;  /* from bsdthread_register */
static int g_only_interp;
static unsigned long g_st_created, g_st_finished, g_st_yielded, g_st_free;
static uint64_t g_st_wait, g_st_empty, g_empty_since;   /* time: waited for the lock; lock free and nobody waiting */

void thread_start(Cpu *main_cpu, int only_interp)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    g_slice = (uint64_t)SLICE_US * 1000 * tb.denom / tb.numer;
    g_thread_cur = main_cpu;
    g_only_interp = only_interp;
    g_since = mach_absolute_time();
}

/* take the lock (g_m held); yield: only after another thread has had it once (yielding) */
static void take_l(Cpu *c, int yield)
{
    unsigned w = g_switch;
    uint64_t t0 = mach_absolute_time();
    if (!g_taken && g_empty_since) { g_st_empty += t0 - g_empty_since; g_empty_since = 0; }
    g_thread_waiting++;
    while (g_taken || (yield && g_switch == w && g_thread_waiting > 1)) pthread_cond_wait(&g_cv, &g_m);
    g_thread_waiting--;
    g_taken = 1;
    g_switch++;
    g_thread_cur = c;
    g_since = mach_absolute_time();
    g_st_wait += g_since - t0;
    if (g_empty_since) { g_st_empty += g_since - g_empty_since; g_empty_since = 0; }
}

static void yield_l(void)
{
    g_taken = 0;
    if (!g_thread_waiting) g_empty_since = mach_absolute_time();
    pthread_cond_broadcast(&g_cv);
}

Cpu *glock_release(void)
{
    Cpu *c = g_thread_cur;
    if (g_threads <= 1) return 0;            /* alone: nobody who needs the lock (only the holder creates new threads) */
    pthread_mutex_lock(&g_m);
    yield_l();
    pthread_mutex_unlock(&g_m);
    g_st_free++;
    return c;
}

void glock_acquire(Cpu *c)
{
    if (!c) return;
    pthread_mutex_lock(&g_m);
    take_l(c, 0);
    pthread_mutex_unlock(&g_m);
    jit_thread_active(c);
}

void thread_release(void)
{
    pthread_mutex_lock(&g_m);
    yield_l();
    pthread_mutex_unlock(&g_m);
}

void thread_yield(Cpu *c)
{
    int immediate = c->yield;
    c->yield = 0;
    if (!g_thread_waiting) {
        if (immediate) sched_yield();          /* held spin_lock, the holder is in a system call */
        return;
    }
    if (!immediate && mach_absolute_time() - g_since < g_slice) return;
    pthread_mutex_lock(&g_m);
    yield_l();
    take_l(c, 1);
    pthread_mutex_unlock(&g_m);
    jit_thread_active(c);
    g_st_yielded++;
}

/* ---------- watchdog ----------
 * Every 4 ms: if someone is waiting (a thread for the lock, an unblocked signal), the running thread is in
 * translated code and it has not been in the dispatcher since the last round (twice in a row), it is stopped
 * (jit_interrupt): at the next block start it enters the dispatcher, which lets others go first or delivers. */
static int g_watchdog_on;
static unsigned long g_st_watchdog;

static void *watchdog(void *arg)
{
    unsigned prev = g_dispatcher_rounds;
    int jam = 0;
    (void)arg;
    for (;;) {
        unsigned v;
        int who;
        usleep(4000);
        v = g_dispatcher_rounds;
        who = g_thread_waiting || (g_sig_pending & ~g_sig_mask);
        if (who && v == prev && g_in_code && !g_patched) {
            if (++jam >= 2) { if (jit_interrupt()) g_st_watchdog++; jam = 0; }
        } else jam = 0;
        prev = v;
    }
    return 0;
}

void thread_watchdog_start(void)
{
    pthread_t t;
    pthread_attr_t at;
    if (g_watchdog_on || g_only_interp || getenv("ROSETTA_NO_WATCHDOG")) return;
    g_watchdog_on = 1;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &at, watchdog, 0);
    pthread_attr_destroy(&at);
}

int thread_register(uint32_t start, uint32_t wqstart, uint32_t pthsize)
{
    g_start = start;
    g_wqstart = wqstart;
    g_pthsize = pthsize;
    return 0;
}

/* host thread of a guest thread: take the lock, run guest code, clean up at the end */
static void *thread_main(void *arg)
{
    Cpu *c = (Cpu *)arg;
    int r;
    glock_acquire(c);
    r = g_only_interp ? run(c) : jit_thread_run(c);
    if (!c->thread_end) main_end(c, r);   /* exit (or error) from this thread: whole process */
    jit_thread_unregister(c);
    g_threads--;
    g_st_finished++;
    free(c->msg);
    free(c);
    pthread_mutex_lock(&g_m);
    yield_l();
    pthread_mutex_unlock(&g_m);
    return 0;
}

int thread_spawn(Cpu *c, uint32_t fn, uint32_t arg, uint32_t stack, uint32_t pthread, uint32_t flags, uint32_t *val)
{
    Cpu *n;
    uint32_t th_stack, th_pthread;
    pthread_t t;
    pthread_attr_t at;
    mach_port_t port;
    (void)c;
    if (!g_start) return EINVAL;            /* bsdthread_register was missing */
    if (!(n = valloc(sizeof *n))) return ENOMEM;   /* page-aligned (xmm, AltiVec) */
    memset(n, 0, sizeof *n);
    flags_set(n, F_IF);
    n->fpu_ctl = 0x037f;
    n->mxcsr = 0x1f80;
    if (!(flags & PTHREAD_START_CUSTOM)) {  /* create the stack with guard page and the pthread structure, like the kernel */
        uint32_t sz = (stack + GUARD + g_pthsize + 4095) & ~4095u;
        void *a = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (a == MAP_FAILED) { free(n); return ENOMEM; }
        mprotect(a, GUARD, PROT_NONE);
        th_stack = (uint32_t)(uintptr_t)a + GUARD + stack;
        th_pthread = th_stack;
    } else {
        th_stack = stack;
        th_pthread = pthread;
    }
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 4u << 20);     /* translator and dispatcher run on the host stack */
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, thread_main, n)) { pthread_attr_destroy(&at); free(n); return EAGAIN; }
    pthread_attr_destroy(&at);
    /* the new thread waits for the lock (this thread holds it): set the registers now */
    port = pthread_mach_thread_np(t);
    mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, 1);  /* reference for the guest */
    n->eip = g_start;
    n->r[EAX] = th_pthread;
    n->r[EBX] = port;
    n->r[ECX] = fn;
    n->r[EDX] = arg;
    n->r[EDI] = stack;
    n->r[ESI] = flags;
    n->r[ESP] = th_stack - 16;
    g_threads++;
    g_st_created++;
    *val = th_pthread;
    thread_watchdog_start();
    return 0;
}

void thread_finish(Cpu *c, uint32_t stack, uint32_t size, uint32_t port, uint32_t sem)
{
    if (size) {                          /* free the stack (libSystem computes what may go) */
        jit_flush_schedule(stack, size);
        munmap((void *)(uintptr_t)stack, size);
    }
    if (port) mach_port_deallocate(mach_task_self(), port);
    if (sem) semaphore_signal(sem);          /* pthread_join waits for this */
    c->thread_end = 1;
    c->finished = 1;
}

/* ---------- kernel work queues ----------
 * NSOperationQueue (10.5) queues items via pthread_workqueue_additem_np; libSystem registers each one with the
 * kernel via workq_ops(QUEUE_ADD, item, prio). The kernel runs a worker thread in _pthread_wqthread (registered with
 * bsdthread_register) with eax = pthread_t (stack end), ebx = port, ecx = stack start, edx = item, edi = reused,
 * esp = stack end - 16 (xnu-1228 setup_wqthread). When the item is done, the thread calls
 * workq_ops(THREAD_RETURN): it gets the next one (reused = 1) or rests until one arrives. */
#define WQ_STACK 0x80000u                  /* PTH_DEFAULT_STACKSIZE */
#define WQ_THREADS 8
#define WQOPS_QUEUE_ADD 1
#define WQOPS_QUEUE_REMOVE 2
#define WQOPS_THREAD_RETURN 4
#define WQOPS_THREAD_SETCONC 8

typedef struct { Cpu *c; uint32_t bottom, top, port; } WqThread;
static struct { uint32_t ent, prio; } g_wq[1024];   /* waiting items (under the lock) */
static int g_nwq, g_wq_threads, g_wq_idle;
static pthread_cond_t g_wq_cv = PTHREAD_COND_INITIALIZER;   /* with g_m */
static unsigned long g_st_wq;

int thread_wq_open(void)
{
    return g_wqstart ? 0 : ENOTSUP;
}

/* next item (lock held); none there: rest, lock free */
static uint32_t wq_fetch(Cpu *c)
{
    uint32_t e;
    int i, best = 0;
    pthread_mutex_lock(&g_m);
    while (!g_nwq) {
        g_wq_idle++;
        yield_l();
        pthread_cond_wait(&g_wq_cv, &g_m);
        g_wq_idle--;
        take_l(c, 0);
    }
    for (i = 1; i < g_nwq; i++) if (g_wq[i].prio < g_wq[best].prio) best = i;   /* 0 high, 1 normal, 2 low */
    e = g_wq[best].ent;
    memmove(&g_wq[best], &g_wq[best + 1], (size_t)(g_nwq - best - 1) * sizeof g_wq[0]);
    g_nwq--;
    pthread_mutex_unlock(&g_m);
    jit_thread_active(c);
    return e;
}

static void *wq_thread(void *arg)
{
    WqThread *w = (WqThread *)arg;
    Cpu *c = w->c;
    int again = 0, r;
    glock_acquire(c);
    for (;;) {
        uint32_t e = wq_fetch(c);
        c->eip = g_wqstart;
        c->r[EAX] = w->top;
        c->r[EBX] = w->port;
        c->r[ECX] = w->bottom;
        c->r[EDX] = e;
        c->r[EDI] = (uint32_t)again;
        c->r[ESI] = 0;
        c->r[EBP] = 0;
        c->r[ESP] = w->top - 16;
        c->finished = 0;
        c->wq_done = 0;
        c->fpu_ctl = 0x037f;
        c->mxcsr = 0x1f80;
        flags_set(c, F_IF);
        g_st_wq++;
        r = g_only_interp ? run(c) : again ? jit_thread_cont(c) : jit_thread_run(c);
        if (!c->wq_done) {
            if (!c->thread_end) main_end(c, r);   /* exit from the worker thread */
            jit_thread_unregister(c);              /* has terminated itself with bsdthread_terminate */
            g_threads--;
            g_wq_threads--;
            thread_release();
            return 0;
        }
        again = 1;
    }
}

static int wq_thread_new(void)
{
    WqThread *w = calloc(1, sizeof *w);
    Cpu *n = valloc(sizeof *n);
    uint32_t sz = (WQ_STACK + GUARD + g_pthsize + 4095) & ~4095u;
    void *a;
    pthread_t t;
    pthread_attr_t at;
    if (!w || !n) { free(w); free(n); return ENOMEM; }
    memset(n, 0, sizeof *n);
    a = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (a == MAP_FAILED) { free(w); free(n); return ENOMEM; }
    mprotect(a, GUARD, PROT_NONE);
    w->c = n;
    w->bottom = (uint32_t)(uintptr_t)a + GUARD;
    w->top = w->bottom + WQ_STACK;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 4u << 20);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, wq_thread, w)) { pthread_attr_destroy(&at); munmap(a, sz); free(w); free(n); return EAGAIN; }
    pthread_attr_destroy(&at);
    w->port = pthread_mach_thread_np(t);    /* the thread is still waiting for the lock */
    mach_port_mod_refs(mach_task_self(), w->port, MACH_PORT_RIGHT_SEND, 1);
    g_threads++;
    g_wq_threads++;
    g_st_created++;
    thread_watchdog_start();
    return 0;
}

int thread_wq_ops(Cpu *c, uint32_t kind, uint32_t ent, uint32_t prio)
{
    int i;
    switch (kind) {
    case WQOPS_QUEUE_ADD:
        if (g_nwq >= (int)(sizeof g_wq / sizeof g_wq[0])) return EAGAIN;
        g_wq[g_nwq].ent = ent;
        g_wq[g_nwq].prio = prio;
        g_nwq++;
        pthread_mutex_lock(&g_m);
        if (g_wq_idle) pthread_cond_signal(&g_wq_cv);
        pthread_mutex_unlock(&g_m);
        if (!g_wq_idle && g_wq_threads < WQ_THREADS) return wq_thread_new();
        return 0;
    case WQOPS_QUEUE_REMOVE:
        for (i = 0; i < g_nwq; i++)
            if (g_wq[i].ent == ent) {
                memmove(&g_wq[i], &g_wq[i + 1], (size_t)(g_nwq - i - 1) * sizeof g_wq[0]);
                g_nwq--;
                return 0;
            }
        return ESRCH;                       /* already running or done */
    case WQOPS_THREAD_RETURN:               /* item done: back into the loop of the worker thread */
        c->wq_done = 1;
        c->finished = 1;
        return 0;
    case WQOPS_THREAD_SETCONC:
        return 0;
    }
    return EINVAL;
}

/* child after fork: only the calling thread exists, and it holds the lock. The mutex may have been held by another
   thread at the moment of the fork, so it is created anew; the watchdog thread did not survive either. */
void thread_after_fork(Cpu *c)
{
    int watchdog_was_on = g_watchdog_on;
    pthread_mutex_init(&g_m, 0);
    pthread_cond_init(&g_cv, 0);
    pthread_cond_init(&g_wq_cv, 0);
    g_taken = 1;
    g_threads = 1;
    g_thread_waiting = 0;
    g_thread_cur = c;
    g_nwq = g_wq_threads = g_wq_idle = 0;
    g_empty_since = 0;
    g_watchdog_on = 0;
    if (watchdog_was_on) thread_watchdog_start();
}

void thread_stats(void)
{
    if (g_watchdog_on) fprintf(stderr, "rosetta: watchdog stopped a thread in a loop %lu times\n", g_st_watchdog);
    if (g_st_wq)
        fprintf(stderr, "rosetta: work queues: %lu items in %d threads\n", g_st_wq, g_wq_threads);
    if (g_st_created) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        fprintf(stderr, "rosetta: threads: %lu created, %lu terminated, lock released %lu times in system calls, %lu times yielded; "
                "waited %.1f ms (all threads together), lock unused %.1f ms\n", g_st_created, g_st_finished, g_st_free,
                g_st_yielded, (double)g_st_wait * tb.numer / tb.denom / 1e6, (double)g_st_empty * tb.numer / tb.denom / 1e6);
    }
}
