/* faults.c - synchronous exceptions reach the program's handlers with the right signal, code and address
 * (i386 only: compared against tests/faults.expected). Division by zero and overflow (SIGFPE), ud2 (SIGILL),
 * int3 (SIGTRAP, continues after it), a handler that skips the faulting instruction via the saved context.
 * Memory faults (writing a read-only page, reading a page without access and address 8, in a hot loop that runs
 * translated, a handler that continues in another function). "faults retry": the handler makes the page writable and returns,
 * the write is repeated (exact state: interpreter; translated, the program ends with the signal instead). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/ucontext.h>

static sigjmp_buf g_back;
static volatile int g_sig, g_code, g_skip, g_count, g_redirect;
static volatile unsigned g_addr, g_fault_addr;
static volatile unsigned char g_byte;
static char *g_page;

static void recovered(void)                   /* the handler continues here instead of at the fault */
{
    printf("continued in another function after signal %d\n", g_sig);
    siglongjmp(g_back, 1);
}

static void on_fault(int sig, siginfo_t *si, void *k)
{
    ucontext_t *u = (ucontext_t *)k;
    g_sig = sig;
    g_code = si->si_code;
    g_addr = (unsigned)u->uc_mcontext->__ss.__eip;
    g_fault_addr = (unsigned)si->si_addr;
    if (sig == SIGSEGV || sig == SIGBUS) {
        if (g_redirect) { u->uc_mcontext->__ss.__eip = (unsigned)recovered; return; }
        siglongjmp(g_back, 1);
    }
    g_byte = *(unsigned char *)g_addr;
    if (g_skip) {                            /* idiv %reg is two bytes: continue after it with eax = 42 */
        u->uc_mcontext->__ss.__eip += 2;
        u->uc_mcontext->__ss.__eax = 42;
        return;
    }
    if (sig == SIGTRAP) return;               /* trap: eip is already after int3 */
    siglongjmp(g_back, 1);
}

static void on_retry(int sig, siginfo_t *si, void *k)
{
    (void)k;
    printf("handler: signal %d at the page %s, making it writable\n", sig,
           ((unsigned)si->si_addr & ~4095u) == (unsigned)g_page ? "yes" : "no");
    mprotect(g_page, 4096, PROT_READ | PROT_WRITE);
}

static int divide(int n, int d)
{
    int q, r;
    __asm__ volatile("cltd; idivl %3" : "=a"(q), "=&d"(r) : "a"(n), "r"(d) : "cc");
    return q;
}

/* reads on until the end of the mapping (hot: translated after a few hundred iterations; volatile, otherwise the
   compiler drops the loads of a function that never returns) */
static int sum_until_fault(const volatile int *a)
{
    int s = 0, i;
    for (i = 0; ; i++) {
        s += a[i];
        g_count = i + 1;
    }
    return s;
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    volatile int r = 0;
    char *two;
    memset(&sa, 0, sizeof sa);
    setvbuf(stdout, 0, _IONBF, 0);
    sa.sa_flags = SA_SIGINFO;
    if (argc > 1 && !strcmp(argv[1], "retry")) {
        g_page = mmap(0, 4096, PROT_READ, MAP_ANON | MAP_PRIVATE, -1, 0);
        sa.sa_sigaction = on_retry;
        sigaction(SIGSEGV, &sa, 0);
        sigaction(SIGBUS, &sa, 0);
        g_page[100] = 7;
        printf("repeated write: value %d\n", g_page[100]);
        return 0;
    }
    sa.sa_sigaction = on_fault;
    sigaction(SIGFPE, &sa, 0);
    sigaction(SIGILL, &sa, 0);
    sigaction(SIGTRAP, &sa, 0);
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGBUS, &sa, 0);

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

    /* memory faults: two pages, the second one read-only, then without access (unmapped memory is no reliable test
       under rosetta: guest and translator share the address space, a page the guest gives back can be reused) */
    two = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    memset(two, 1, 8192);
    mprotect(two + 4096, 4096, PROT_READ);
    if (!sigsetjmp(g_back, 1)) { two[4096 + 8] = 2; printf("no exception writing a read-only page\n"); }
    else printf("write to a read-only page: signal %d, code %d, address right %d\n", g_sig, g_code,
                g_fault_addr == (unsigned)(two + 4096 + 8));
    mprotect(two + 4096, 4096, PROT_NONE);
    if (!sigsetjmp(g_back, 1)) { r = two[4096 + 16]; printf("no exception reading a page without access\n"); }
    else printf("read of a page without access: signal %d, code %d, address right %d\n", g_sig, g_code,
                g_fault_addr == (unsigned)(two + 4096 + 16));
    if (!sigsetjmp(g_back, 1)) { r = *(volatile int *)8; printf("no exception reading address 8\n"); }
    else printf("read of address 8: signal %d, address right %d\n", g_sig, g_fault_addr == 8);
    g_count = 0;
    if (!sigsetjmp(g_back, 1)) { r = sum_until_fault((const volatile int *)two); printf("no exception at the end of the mapping\n"); }
    else printf("loop over the end of the mapping: signal %d, %d elements read, address right %d\n", g_sig, g_count,
                g_fault_addr == (unsigned)(two + 4096));
    g_redirect = 1;
    if (!sigsetjmp(g_back, 1)) { r = two[4096 + 24]; printf("no exception reading a page without access\n"); }
    g_redirect = 0;
    {
        sigset_t m;
        sigprocmask(SIG_BLOCK, 0, &m);
        printf("after the memory faults: SIGSEGV blocked %d, SIGBUS blocked %d\n", sigismember(&m, SIGSEGV), sigismember(&m, SIGBUS));
    }
    return 0;
}
