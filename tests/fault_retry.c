/* fault_retry.c - a SIGSEGV/SIGBUS handler that returns to retry the access, the way SheepShaver tracks writes to its
 * screen (VOSF) and garbage collectors their write barriers: pages are made read-only, the first write to each one
 * faults, the handler records the page, allows writing and returns, the write happens. In hot loops, so the faulting
 * stores are translated code. The output must equal the native PowerPC build's. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/mman.h>

#define PAGES 16
static unsigned char *g_mem;
static volatile int g_faults, g_dirty[PAGES];
static sigjmp_buf g_jump;
static volatile int g_jump_on;

static void handler(int sig, siginfo_t *si, void *ctx)
{
    unsigned char *a = (unsigned char *)si->si_addr;
    long k = (a - g_mem) / 4096;
    (void)sig; (void)ctx;
    if (g_jump_on) { g_jump_on = 0; siglongjmp(g_jump, 1); }      /* a probe: jump out instead of returning */
    if (k < 0 || k >= PAGES) { static const char m[] = "fault outside\n"; write(1, m, sizeof m - 1); _exit(2); }
    g_faults++;
    g_dirty[k]++;
    mprotect(g_mem + 4096 * k, 4096, PROT_READ | PROT_WRITE);
}

static unsigned sum(void)
{
    unsigned s = 0;
    int i;
    for (i = 0; i < 4096 * PAGES; i++) s = s * 31 + g_mem[i];
    return s;
}

int main(void)
{
    struct sigaction sa;
    int round, i, k;
    volatile long x = 0;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGBUS, &sa, 0);
    g_mem = mmap(0, 4096 * PAGES, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    for (round = 0; round < 60; round++) {
        mprotect(g_mem, 4096 * PAGES, PROT_READ);                    /* the screen is clean again */
        for (i = 0; i < 20000; i++) {                                 /* byte, word and block writes */
            int p = (i * 7 + round * 3) % (4096 * PAGES);
            g_mem[p] += (unsigned char)(i + round);
            if ((i & 7) == 0) *(unsigned *)(g_mem + (p & ~3)) = (unsigned char)i * 0x01010101u;   /* (same in either byte order) */
        }
        if (round % 10 == 0) memset(g_mem + 4096 * (round / 10), round, 100);
    }
    printf("faults %d\n", g_faults);
    for (k = 0; k < PAGES; k++) printf("%d%c", g_dirty[k], k == PAGES - 1 ? '\n' : ' ');
    printf("sum %08x\n", sum());
    /* a handler that jumps out (siglongjmp) of a translated loop still works */
    mprotect(g_mem, 4096, PROT_READ);
    for (i = 0; i < 3; i++) {
        if (sigsetjmp(g_jump, 1) == 0) {
            g_jump_on = 1;
            for (k = 0; k < 100000; k++) { x += k; if (k == 50000 + i) g_mem[i] = 1; }
        } else printf("jumped out %d, x %ld\n", i, x);
    }
    return 0;
}
