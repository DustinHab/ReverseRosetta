/* interrupt.c - loops without a system call must be interruptible: a signal (SIGALRM) in a tight loop and in one
 * with a call; a thread that waits for another without pause (tight and with a call, across several blocks). */
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>

static volatile int g_sig, g_flag;
static volatile unsigned g_n;

static void catcher(int s) { (void)s; g_sig = 1; }

__attribute__((noinline)) static unsigned step(unsigned x) { return x * 1103515245u + 12345u; }

static void on_alarm(int ms)
{
    struct itimerval it = { { 0, 0 }, { 0, 0 } };
    it.it_value.tv_usec = ms * 1000;
    g_sig = 0;
    setitimer(ITIMER_REAL, &it, 0);
}

static void *setter(void *a)
{
    usleep((useconds_t)(long)a);
    g_flag = 1;
    return 0;
}

int main(void)
{
    pthread_t t;
    unsigned x = 1;
    signal(SIGALRM, catcher);

    on_alarm(20);
    while (!g_sig) g_n++;                   /* tight, one block */
    printf("signal in tight loop: ok\n");

    on_alarm(20);
    while (!g_sig) x = step(x);          /* with a call */
    printf("signal in loop with call: ok\n");

    on_alarm(20);
    while (!g_sig) { if (x & 1) x = x * 3 + 1; else x >>= 1; g_n += x & 7; }   /* branches */
    printf("signal in branching loop: ok\n");

    g_flag = 0;
    pthread_create(&t, 0, setter, (void *)5000);
    while (!g_flag) g_n++;                /* thread waits without pause, without a system call */
    pthread_join(t, 0);
    printf("waiting for thread, tight: ok\n");

    g_flag = 0;
    pthread_create(&t, 0, setter, (void *)5000);
    while (!g_flag) x = step(x);
    pthread_join(t, 0);
    printf("waiting for thread with call: ok\n");
    return (int)(x & 0);
}
