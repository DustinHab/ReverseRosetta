/* threads.c - pthreads: mutex, condition variable, OSSpinLock, join, detach, usleep, pipe between threads, keys.
 * Output is deterministic (sums, order via join), compared with the native PowerPC build. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <libkern/OSAtomic.h>

#define N 4
#define ROUND 20000

static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static long g_counter, g_spin_counter;
static OSSpinLock g_spin = OS_SPINLOCK_INIT;
static volatile int32_t g_atom;
static int g_ready, g_round;
static pthread_key_t g_key;

/* computation + shared counter under mutex, spinlock and atomic */
static void *worker(void *a)
{
    long nr = (long)a, i, sum = 0;
    pthread_setspecific(g_key, (void *)(nr * 100));
    for (i = 0; i < ROUND; i++) {
        sum += (i * (nr + 3)) % 7;
        pthread_mutex_lock(&g_m);
        g_counter++;
        pthread_mutex_unlock(&g_m);
        OSSpinLockLock(&g_spin);
        g_spin_counter += 2;
        OSSpinLockUnlock(&g_spin);
        OSAtomicIncrement32(&g_atom);
        if ((i & 1023) == 0) sched_yield();
    }
    if ((long)pthread_getspecific(g_key) != nr * 100) sum = -1;
    return (void *)sum;
}

/* condition variable: ping-pong via g_round */
static void *pong(void *a)
{
    int k;
    (void)a;
    for (k = 0; k < 50; k++) {
        pthread_mutex_lock(&g_m);
        while (g_round % 2 == 0) pthread_cond_wait(&g_cv, &g_m);
        g_round++;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_m);
    }
    return 0;
}

static int g_pipe[2];
static void *writer(void *a)
{
    int k;
    char z[32];
    (void)a;
    for (k = 0; k < 20; k++) {
        usleep(500);
        snprintf(z, sizeof z, "line %d\n", k);
        write(g_pipe[1], z, strlen(z));
    }
    close(g_pipe[1]);
    return 0;
}

static void *endless(void *a)
{
    (void)a;
    for (;;) usleep(1000);
    return 0;
}

static void *detached(void *a)
{
    (void)a;
    pthread_mutex_lock(&g_m);
    g_ready = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_m);
    return 0;
}

int main(void)
{
    pthread_t t[N], p, s, d;
    pthread_attr_t at;
    long i, total = 0;
    int k, nlines = 0;
    char buf[256];
    ssize_t n;

    pthread_key_create(&g_key, 0);
    for (i = 0; i < N; i++) pthread_create(&t[i], 0, worker, (void *)i);
    for (i = 0; i < N; i++) {
        void *r;
        pthread_join(t[i], &r);
        printf("thread %ld: sum %ld\n", i, (long)r);
        total += (long)r;
    }
    printf("mutex counter %ld (expected %d), spinlock %ld (expected %d), atomic %d\n", g_counter, N * ROUND, g_spin_counter,
           2 * N * ROUND, (int)g_atom);

    pthread_create(&p, 0, pong, 0);
    for (k = 0; k < 50; k++) {
        pthread_mutex_lock(&g_m);
        while (g_round % 2 == 1) pthread_cond_wait(&g_cv, &g_m);
        g_round++;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_m);
    }
    pthread_join(p, 0);
    printf("ping-pong: round %d\n", g_round);

    pipe(g_pipe);
    pthread_create(&s, 0, writer, 0);
    while ((n = read(g_pipe[0], buf, sizeof buf - 1)) > 0) {
        buf[n] = 0;
        for (k = 0; k < n; k++) if (buf[k] == '\n') nlines++;
    }
    pthread_join(s, 0);
    printf("pipe: %d lines\n", nlines);

    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&d, &at, detached, 0);
    pthread_mutex_lock(&g_m);
    while (!g_ready) pthread_cond_wait(&g_cv, &g_m);
    pthread_mutex_unlock(&g_m);
    printf("detached thread done\n");

    for (k = 0; k < 3; k++) {               /* many short threads in a row (reuse ring buffers and stacks) */
        for (i = 0; i < N; i++) pthread_create(&t[i], 0, worker, (void *)i);
        for (i = 0; i < N; i++) pthread_join(t[i], 0);
    }
    printf("mutex counter %ld, total sum %ld\n", g_counter, total);
    pthread_create(&d, &at, endless, 0);     /* still running when main ends: the process must end anyway */
    usleep(3000);
    return 0;
}
