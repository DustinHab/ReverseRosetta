/* signal_regs.c - whether floating-point and vector registers survive signal delivery (Leopard, G4).
 *
 *   signal_regs [us] [seconds] [vrsave]
 *
 * Fills f14..f31 and v0..v3 with known values, sets VRSAVE (default 0xf0000000 like the translator) and checks
 * them in a loop while SIGALRM arrives every us microseconds. Reports every mismatch with the value found
 * (0x7FFFDEAD = fill value of the kernel for vector registers, 0xC24BC195 87859393 for floating-point registers).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>

static volatile unsigned long g_signals;

static void sample(int sig, siginfo_t *info, void *k)
{
    (void)sig; (void)info; (void)k;
    g_signals++;
}

/* runs the given number of rounds: load registers, wait, compare. Returns the number of errors; the first wrong
   value goes to *fw */
static unsigned long must_check(unsigned long rounds, unsigned vrsave, unsigned *fw, int *fwo)
{
    static double fwant[18] __attribute__((aligned(16)));
    static unsigned vwant[16] __attribute__((aligned(16)));
    static double fist[18] __attribute__((aligned(16)));
    static unsigned vist[16] __attribute__((aligned(16)));
    unsigned long errnum = 0, r;
    int i;
    for (i = 0; i < 18; i++) fwant[i] = 1000.0 + i * 3.25;
    for (i = 0; i < 16; i++) vwant[i] = 0x11110000u + (unsigned)i * 0x01010101u;
    for (r = 0; r < rounds; r++) {
        __asm__ volatile(
            "mfspr r0, 256\n\t"
            "stw r0, -4(r1)\n\t"
            "mtspr 256, %4\n\t"
            "lfd f14, 0(%0)\n\t lfd f15, 8(%0)\n\t lfd f16, 16(%0)\n\t lfd f17, 24(%0)\n\t"
            "lfd f18, 32(%0)\n\t lfd f19, 40(%0)\n\t lfd f20, 48(%0)\n\t lfd f21, 56(%0)\n\t"
            "lfd f22, 64(%0)\n\t lfd f23, 72(%0)\n\t lfd f24, 80(%0)\n\t lfd f25, 88(%0)\n\t"
            "lfd f26, 96(%0)\n\t lfd f27, 104(%0)\n\t lfd f28, 112(%0)\n\t lfd f29, 120(%0)\n\t"
            "lfd f30, 128(%0)\n\t lfd f31, 136(%0)\n\t"
            "li r0, 0\n\t lvx v0, %1, r0\n\t li r0, 16\n\t lvx v1, %1, r0\n\t"
            "li r0, 32\n\t lvx v2, %1, r0\n\t li r0, 48\n\t lvx v3, %1, r0\n\t"
            "lis r0, 0x10\n\t mtctr r0\n"
            "1: bdnz 1b\n\t"                                  /* wait ~1 million cycles: signals hit here */
            "stfd f14, 0(%2)\n\t stfd f15, 8(%2)\n\t stfd f16, 16(%2)\n\t stfd f17, 24(%2)\n\t"
            "stfd f18, 32(%2)\n\t stfd f19, 40(%2)\n\t stfd f20, 48(%2)\n\t stfd f21, 56(%2)\n\t"
            "stfd f22, 64(%2)\n\t stfd f23, 72(%2)\n\t stfd f24, 80(%2)\n\t stfd f25, 88(%2)\n\t"
            "stfd f26, 96(%2)\n\t stfd f27, 104(%2)\n\t stfd f28, 112(%2)\n\t stfd f29, 120(%2)\n\t"
            "stfd f30, 128(%2)\n\t stfd f31, 136(%2)\n\t"
            "li r0, 0\n\t stvx v0, %3, r0\n\t li r0, 16\n\t stvx v1, %3, r0\n\t"
            "li r0, 32\n\t stvx v2, %3, r0\n\t li r0, 48\n\t stvx v3, %3, r0\n\t"
            "lwz r0, -4(r1)\n\t"
            "mtspr 256, r0\n\t"
            : : "b"(fwant), "b"(vwant), "b"(fist), "b"(vist), "r"(vrsave)
            : "r0", "ctr", "memory", "f14", "f15", "f16", "f17", "f18", "f19", "f20", "f21", "f22", "f23", "f24",
              "f25", "f26", "f27", "f28", "f29", "f30", "f31", "v0", "v1", "v2", "v3");
        if (memcmp(fwant, fist, sizeof fwant) || memcmp(vwant, vist, sizeof vwant)) {
            if (!errnum) {
                for (i = 0; i < 18; i++)
                    if (fwant[i] != fist[i]) { memcpy(fw, &fist[i], 8); *fwo = 14 + i; break; }
                if (i == 18)
                    for (i = 0; i < 16; i++) if (vwant[i] != vist[i]) { fw[0] = vist[i]; fw[1] = 0; *fwo = 100 + i / 4; break; }
            }
            errnum++;
        }
    }
    return errnum;
}

int main(int argc, char **argv)
{
    int us = argc > 1 ? atoi(argv[1]) : 29, sec = argc > 2 ? atoi(argv[2]) : 5, fwo = -1;
    unsigned vrsave = argc > 3 ? (unsigned)strtoul(argv[3], 0, 0) : 0xf0000000u, fw[2] = { 0, 0 };
    struct sigaction sa;
    struct itimerval it;
    struct timeval t0, t1;
    unsigned long rounds = 0, errnum = 0;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGALRM, &sa, 0);
    memset(&it, 0, sizeof it);
    if (us > 0) { it.it_interval.tv_usec = it.it_value.tv_usec = us; setitimer(ITIMER_REAL, &it, 0); }
    gettimeofday(&t0, 0);
    do {
        errnum += must_check(50, vrsave, fw, &fwo);
        rounds += 50;
        gettimeofday(&t1, 0);
    } while (t1.tv_sec - t0.tv_sec < sec);
    printf("%lu rounds, %lu signals, %lu rounds with wrong registers", rounds, g_signals, errnum);
    if (errnum) printf(" (first %s%d = %08x %08x)", fwo >= 100 ? "v" : "f", fwo >= 100 ? fwo - 100 : fwo, fw[0], fw[1]);
    printf("\n");
    return errnum != 0;
}
