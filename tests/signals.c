/* signals.c - signals to the program: own handler (sigaction, with and without SA_SIGINFO), raise, alarm,
   signal mask, ignoring SIGPIPE. Output compared with the native build */
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

static volatile int g_usr1, g_alrm, g_info_sig, g_info_code;

static void at_usr1(int sig) { g_usr1 += sig; }
static void at_alrm(int sig) { (void)sig; g_alrm++; }
static void at_info(int sig, siginfo_t *si, void *k) { (void)k; g_info_sig = sig; g_info_code = si ? si->si_signo : -1; }

int main(void)
{
    struct sigaction sa;
    sigset_t m, prev;
    int fd[2], i;
    volatile double x = 1.0;
    signal(SIGUSR1, at_usr1);
    raise(SIGUSR1);
    raise(SIGUSR1);
    printf("usr1 %d\n", g_usr1);
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = at_info;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR2, &sa, 0);
    kill(getpid(), SIGUSR2);
    printf("info %d %d\n", g_info_sig, g_info_code);
    sigemptyset(&m);
    sigaddset(&m, SIGUSR1);
    sigprocmask(SIG_BLOCK, &m, &prev);
    raise(SIGUSR1);
    printf("blocked %d\n", g_usr1);
    sigprocmask(SIG_SETMASK, &prev, 0);
    printf("unblocked %d\n", g_usr1);
    signal(SIGALRM, at_alrm);
    alarm(1);
    pause();                                                        /* sigsuspend */
    printf("alarm %d\n", g_alrm);
    ualarm(200000, 0);
    for (i = 0; i < 100 && g_alrm < 2; i++) { usleep(20000); x = x * 1.5; }   /* system calls until it arrives */
    printf("alarm %d %s\n", g_alrm, x > 1.0 ? "computed" : "?");
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = at_usr1;
    sa.sa_flags = SA_RESETHAND;
    sigaction(SIGUSR1, &sa, 0);
    raise(SIGUSR1);
    sigaction(SIGUSR1, 0, &sa);
    printf("reset %d %s\n", g_usr1, sa.sa_handler == SIG_DFL ? "DFL" : "handler");
    signal(SIGPIPE, SIG_IGN);
    pipe(fd);
    close(fd[0]);
    printf("pipe %d\n", (int)write(fd[1], "x", 1));
    return 0;
}
