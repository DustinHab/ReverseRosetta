/* gcall.c - guest functions called from host threads (CoreAudio's I/O thread calls the program's IOProc, listeners).
 *
 * A guest thread of its own (created through the guest's pthread start routine, so it has a real pthread and TLS) runs
 * a small loop in guest code:
 *     loop: mov $GC_WAIT, %eax; int $0x90; call *%eax; mov %eax, %ecx; mov $GC_DONE, %eax; int $0x90; jmp loop
 * At GC_WAIT it gives the thread lock away and waits for a request; then it puts the arguments on its stack (16-byte
 * aligned at the call, as the i386 ABI wants) and returns the function in eax. At GC_DONE it takes the result from ecx,
 * restores its stack and wakes the requester. Requests are served one at a time. A requester that is itself a guest
 * thread holding the lock passes its Cpu and gives the lock away while it waits. */
#include "gcall.h"
#include "thread.h"
#include "interp.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>

#define GC_WAIT 0xc0de0001u
#define GC_DONE 0xc0de0002u
#define GC_ARGS 8

static pthread_mutex_t g_gm = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_serial = PTHREAD_MUTEX_INITIALIZER;   /* one request at a time */
static pthread_cond_t g_gc_req = PTHREAD_COND_INITIALIZER, g_gc_done = PTHREAD_COND_INITIALIZER;
static uint32_t g_stub;                 /* guest address of the loop */
static int g_ready;                     /* the guest thread waits for requests */
static int g_state;                     /* 0 free, 1 requested, 2 running, 3 done */
static uint32_t g_fn, g_args[GC_ARGS], g_result, g_saved_esp;
static int g_nargs;
static Cpu *g_gc_cpu;                    /* the serving thread (it cannot call itself) */
static unsigned long g_st_calls, g_st_late;

int gcall_start(Cpu *c)
{
    static const uint8_t code[] = {
        0xb8, 0x01, 0x00, 0xde, 0xc0,       /* mov $GC_WAIT, %eax */
        0xcd, 0x90,                         /* int $0x90 */
        0xff, 0xd0,                         /* call *%eax */
        0x89, 0xc1,                         /* mov %eax, %ecx */
        0xb8, 0x02, 0x00, 0xde, 0xc0,       /* mov $GC_DONE, %eax */
        0xcd, 0x90,                         /* int $0x90 */
        0xeb, 0xec,                         /* jmp loop */
    };
    uint32_t val;
    void *p;
    if (g_stub) return 0;
    p = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) return ENOMEM;
    memcpy(p, code, sizeof code);
    g_stub = (uint32_t)(uintptr_t)p;
    if (thread_spawn(c, g_stub, 0, 0x40000, 0, 0, &val)) { munmap(p, 4096); g_stub = 0; return EAGAIN; }
    return 0;
}

void gcall_trap(Cpu *c)
{
    if (c->r[EAX] == GC_WAIT) {
        Cpu *me;
        uint32_t esp, k;
        g_gc_cpu = c;
        me = glock_release();
        pthread_mutex_lock(&g_gm);
        g_ready = 1;
        pthread_cond_broadcast(&g_gc_done);           /* (a requester may wait for the thread to be ready) */
        while (g_state != 1) pthread_cond_wait(&g_gc_req, &g_gm);
        g_state = 2;
        pthread_mutex_unlock(&g_gm);
        glock_acquire(me);
        esp = c->r[ESP];
        g_saved_esp = esp;
        esp = (esp - 4 * (uint32_t)g_nargs - 64) & ~15u;
        for (k = 0; k < (uint32_t)g_nargs; k++) st32(esp + 4 * k, g_args[k]);
        c->r[ESP] = esp;
        c->r[EAX] = g_fn;
        return;
    }
    if (c->r[EAX] == GC_DONE) {
        c->r[ESP] = g_saved_esp;
        pthread_mutex_lock(&g_gm);
        g_result = c->r[ECX];
        g_state = 3;
        pthread_cond_broadcast(&g_gc_done);
        pthread_mutex_unlock(&g_gm);
    }
}

static void deadline(struct timespec *t, int ms)
{
    struct timeval now;
    gettimeofday(&now, 0);
    t->tv_sec = now.tv_sec + ms / 1000;
    t->tv_nsec = (long)now.tv_usec * 1000 + (long)(ms % 1000) * 1000000;
    if (t->tv_nsec >= 1000000000) { t->tv_sec++; t->tv_nsec -= 1000000000; }
}

int guest_call(Cpu *c, uint32_t fn, const uint32_t *args, int n, uint32_t *result, int timeout_ms)
{
    struct timespec t;
    Cpu *me = 0;
    int r = 0, k;
    if (!g_stub || n > GC_ARGS || (c && c == g_gc_cpu)) return -1;
    if (c) me = glock_release();
    pthread_mutex_lock(&g_serial);
    pthread_mutex_lock(&g_gm);
    deadline(&t, timeout_ms);
    /* the previous request may still run (it timed out): wait for it, then the thread must be waiting again */
    while ((g_state == 1 || g_state == 2 || !g_ready) && r != ETIMEDOUT) r = pthread_cond_timedwait(&g_gc_done, &g_gm, &t);
    if (g_state == 3) g_state = 0;
    if (r == ETIMEDOUT || !g_ready || g_state != 0) { pthread_mutex_unlock(&g_gm); pthread_mutex_unlock(&g_serial); glock_acquire(me); return -1; }
    g_fn = fn;
    g_nargs = n;
    for (k = 0; k < n; k++) g_args[k] = args[k];
    g_state = 1;
    g_ready = 0;
    pthread_cond_signal(&g_gc_req);
    while (g_state != 3 && r != ETIMEDOUT) r = pthread_cond_timedwait(&g_gc_done, &g_gm, &t);
    if (g_state == 3) {
        if (result) *result = g_result;
        g_state = 0;
        g_st_calls++;
        r = 0;
    } else {
        g_st_late++;                        /* still running: the next request waits for it */
        r = -1;
    }
    pthread_mutex_unlock(&g_gm);
    pthread_mutex_unlock(&g_serial);
    glock_acquire(me);
    return r;
}

void gcall_stats(void)
{
    if (g_st_calls || g_st_late) fprintf(stderr, "rosetta: %lu guest calls from host threads, %lu too late\n", g_st_calls, g_st_late);
}
