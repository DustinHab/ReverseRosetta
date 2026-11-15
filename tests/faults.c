/* faults.c - synchronous exceptions reach the program's handlers with the right signal, code and address
 * (i386 only: compared against tests/faults.expected). Division by zero and overflow (SIGFPE), ud2 (SIGILL),
 * int3 (SIGTRAP, continues after it), a handler that skips the faulting instruction via the saved context. */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <limits.h>
#include <sys/ucontext.h>

static sigjmp_buf g_back;
static volatile int g_sig, g_code, g_skip;
static volatile unsigned g_addr;
static volatile unsigned char g_byte;

static void on_fault(int sig, siginfo_t *si, void *k)
{
    ucontext_t *u = (ucontext_t *)k;
    g_sig = sig;
    g_code = si->si_code;
    g_addr = (unsigned)u->uc_mcontext->__ss.__eip;
    g_byte = *(unsigned char *)g_addr;
    if (g_skip) {                            /* idiv %reg is two bytes: continue after it with eax = 42 */
        u->uc_mcontext->__ss.__eip += 2;
        u->uc_mcontext->__ss.__eax = 42;
        return;
    }
    if (sig == SIGTRAP) return;               /* trap: eip is already after int3 */
    siglongjmp(g_back, 1);
}

static int divide(int n, int d)
{
    int q, r;
    __asm__ volatile("cltd; idivl %3" : "=a"(q), "=&d"(r) : "a"(n), "r"(d) : "cc");
    return q;
}

int main(void)
{
    struct sigaction sa;
    volatile int r = 0;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGFPE, &sa, 0);
    sigaction(SIGILL, &sa, 0);
    sigaction(SIGTRAP, &sa, 0);
    setvbuf(stdout, 0, _IONBF, 0);

    if (!sigsetjmp(g_back, 1)) { r = divide(7, 0); printf("no exception for 7 / 0\n"); }
    else printf("7 / 0: signal %d, code %d, at idiv %s\n", g_sig, g_code, g_byte == 0xf7 ? "yes" : "no");
    if (!sigsetjmp(g_back, 1)) { r = divide(INT_MIN, -1); printf("no exception for INT_MIN / -1\n"); }
    else printf("INT_MIN / -1: signal %d, code %d, at idiv %s\n", g_sig, g_code, g_byte == 0xf7 ? "yes" : "no");
    if (!sigsetjmp(g_back, 1)) { __asm__ volatile("ud2"); printf("no exception for ud2\n"); }
    else printf("ud2: signal %d, code %d, at ud2 %s\n", g_sig, g_code, g_byte == 0x0f ? "yes" : "no");
    g_sig = 0;
    __asm__ volatile("int3");
    printf("int3: signal %d, code %d, continued after it\n", g_sig, g_code);
    g_skip = 1;
    r = divide(100, 0);
    g_skip = 0;
    printf("skipped division: result %d\n", r);
    {   /* blocked handler signal: masks survive the delivery (handler runs with SIGFPE blocked, mask restored) */
        sigset_t m;
        if (!sigsetjmp(g_back, 1)) { r = divide(1, 0); }
        sigprocmask(SIG_BLOCK, 0, &m);
        printf("after siglongjmp: SIGFPE blocked %d\n", sigismember(&m, SIGFPE));
    }
    return 0;
}
