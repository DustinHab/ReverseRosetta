/* syscalls.c - process, signal and file system calls; the output must match the native PowerPC build.
 * fork, vfork, execve (native, translated, script), system, popen, posix_spawn with file actions, waitid,
 * kqueue/kevent, fcntl locks against a child, POSIX semaphores, fifo, truncate, futimes, chflags, setattrlist,
 * blocked signals with and without handler (sigpending, sigwait), terminal settings on a pty, FIONBIO, sysctl
 * structures (kinfo_proc, own arguments, loadavg, swap usage, clock rate, vfsconf, nfsstats), getifaddrs, utmpx,
 * SysV shared memory, getattrlist with directory and file attributes at once, quarantine labels.
 * Prints no pids, times or paths. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <semaphore.h>
#include <termios.h>
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/attr.h>
#include <sys/xattr.h>
#include <sys/resource.h>
#include <sys/ptrace.h>
#include <sys/sysctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <utmpx.h>
#include <libproc.h>
#include <mach/vm_inherit.h>

extern char **environ;

static char tmp[256];
static const char *self;

static void name(const char *what) { snprintf(tmp, sizeof tmp, "/tmp/rr_sys_%d_%s", (int)getpid(), what); }

static int wait_code(pid_t p)
{
    int st = 0;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) ;
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 1000 + WTERMSIG(st);
    return -1;
}

/* second mode: started by execve from main */
static int exec_child(char **argv)
{
    sigset_t m;
    struct sigaction sa;
    sigprocmask(SIG_BLOCK, 0, &m);
    sigaction(SIGUSR1, 0, &sa);
    printf("exec child: argv0 %s, argument %s, mask %s, ignore %s\n", strcmp(argv[0], "custom-name") ? "wrong" : "kept",
           argv[2] ? argv[2] : "-", sigismember(&m, SIGUSR2) ? "inherited" : "lost",
           sa.sa_handler == SIG_IGN ? "inherited" : "lost");
    return 5;
}

static void t_fork(void)
{
    int pfd[2];
    char buf[32];
    pid_t p;
    ssize_t n;
    pipe(pfd);
    p = fork();
    if (p == 0) {
        close(pfd[0]);
        write(pfd[1], "from child", 10);
        _exit(42);
    }
    close(pfd[1]);
    n = read(pfd[0], buf, sizeof buf - 1);
    buf[n > 0 ? n : 0] = 0;
    close(pfd[0]);
    printf("fork: child said \"%s\", exit %d, pid %s\n", buf, wait_code(p), p > 0 && p != getpid() ? "new" : "wrong");
    p = vfork();
    if (p == 0) _exit(3);
    printf("vfork: exit %d\n", wait_code(p));
}

static void on_hup(int sig) { (void)sig; }

static void t_exec(void)
{
    pid_t p;
    sigset_t m, old;
    char *av[4];
    FILE *f;
    p = fork();
    if (p == 0) {
        char *a[] = { "echo", "exec native", 0 };
        execve("/bin/echo", a, environ);
        _exit(99);
    }
    printf("execve native: exit %d\n", wait_code(p));
    signal(SIGHUP, on_hup);                  /* a handler: rosetta's watchdog thread runs, execve must still work */
    p = fork();
    if (p == 0) {
        char *a[] = { "echo", "exec with a signal handler", 0 };
        execve("/bin/echo", a, environ);
        printf("execve with handler: %s\n", strerror(errno));
        _exit(99);
    }
    printf("execve with handler: exit %d\n", wait_code(p));
    signal(SIGHUP, SIG_DFL);
    /* translated (this program again): argv[0], signal mask and ignored signals reach the new program */
    sigemptyset(&m);
    sigaddset(&m, SIGUSR2);
    sigprocmask(SIG_BLOCK, &m, &old);
    signal(SIGUSR1, SIG_IGN);
    p = fork();
    if (p == 0) {
        av[0] = "custom-name"; av[1] = "exec-child"; av[2] = "with argument"; av[3] = 0;
        execve(self, av, environ);
        _exit(98);
    }
    printf("execve self: exit %d\n", wait_code(p));
    signal(SIGUSR1, SIG_DFL);
    sigprocmask(SIG_SETMASK, &old, 0);
    name("script.sh");
    f = fopen(tmp, "w");
    fprintf(f, "#!/bin/sh\necho \"script says $1\"\nexit 6\n");
    fclose(f);
    chmod(tmp, 0755);
    p = fork();
    if (p == 0) {
        char *a[] = { "script", "hello", 0 };
        execve(tmp, a, environ);
        _exit(97);
    }
    printf("execve script: exit %d\n", wait_code(p));
    unlink(tmp);
    errno = 0;
    {
        char *a[] = { "none", 0 };
        execve("/nonexistent/program", a, environ);
        printf("execve missing: %s\n", strerror(errno));
    }
    {
        int st = system("echo system runs; exit 4");
        printf("system: %d\n", WEXITSTATUS(st));
    }
    f = popen("echo popen line; echo second", "r");
    if (f) {
        char line[64];
        int st;
        while (fgets(line, sizeof line, f)) printf("popen: %s", line);
        st = pclose(f);
        printf("pclose: %d\n", WEXITSTATUS(st));
    }
}

static void t_spawn(void)
{
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    pid_t p;
    int pfd[2], r;
    char buf[128];
    ssize_t n, got = 0;
    char *a[] = { "sh", "-c", "echo spawned out; echo spawned err >&2; exit 8", 0 };
    pipe(pfd);
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pfd[1], 1);
    posix_spawn_file_actions_adddup2(&fa, pfd[1], 2);
    posix_spawn_file_actions_addclose(&fa, pfd[0]);
    posix_spawn_file_actions_addclose(&fa, pfd[1]);
    r = posix_spawn(&p, "/bin/sh", &fa, 0, a, environ);
    close(pfd[1]);
    while ((n = read(pfd[0], buf + got, sizeof buf - 1 - got)) > 0) got += n;
    buf[got] = 0;
    close(pfd[0]);
    printf("posix_spawn: %d, exit %d, output: %s", r, r ? -1 : wait_code(p), buf);
    posix_spawn_file_actions_destroy(&fa);
    /* open action and signal attributes */
    name("spawn.txt");
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawnattr_init(&at);
    {
        sigset_t m;
        sigemptyset(&m);
        sigaddset(&m, SIGINT);
        posix_spawnattr_setsigmask(&at, &m);
        posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK);
    }
    {
        char *b[] = { "sh", "-c", "echo into file; trap '' INT; kill -INT $$; echo after blocked signal", 0 };
        r = posix_spawn(&p, "/bin/sh", &fa, &at, b, environ);
        printf("posix_spawn open: %d, exit %d\n", r, r ? -1 : wait_code(p));
    }
    {
        FILE *f = fopen(tmp, "r");
        char line[64];
        while (f && fgets(line, sizeof line, f)) printf("file: %s", line);
        if (f) fclose(f);
    }
    unlink(tmp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    r = posix_spawn(&p, "/nonexistent/program", 0, 0, a, environ);
    printf("posix_spawn missing: %s\n", strerror(r));
}

static void t_waitid(void)
{
    siginfo_t si;
    pid_t p = fork();
    if (p == 0) _exit(9);
    memset(&si, 0, sizeof si);
    if (waitid(P_PID, p, &si, WEXITED) < 0) { printf("waitid: %s\n", strerror(errno)); return; }
    printf("waitid: signo %d, code %s, status %d, pid %s\n", si.si_signo, si.si_code == CLD_EXITED ? "exited" : "other",
           si.si_status, si.si_pid == p ? "right" : "wrong");
}

static void t_kqueue(void)
{
    int kq = kqueue(), pfd[2], n;
    struct kevent ch[2], ev[4];
    struct timespec ts = { 2, 0 };
    pid_t p;
    pipe(pfd);
    EV_SET(&ch[0], pfd[0], EVFILT_READ, EV_ADD, 0, 0, (void *)0x1234);
    EV_SET(&ch[1], 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 30, (void *)0x5678);
    n = kevent(kq, ch, 2, 0, 0, 0);
    printf("kevent add: %d\n", n);
    write(pfd[1], "12345", 5);
    n = kevent(kq, 0, 0, ev, 4, &ts);
    printf("kevent: %d event(s): filter %d, ident %s, data %d, udata %s\n", n, n > 0 ? ev[0].filter : 0,
           n > 0 && (int)ev[0].ident == pfd[0] ? "pipe" : "other", n > 0 ? (int)ev[0].data : 0,
           n > 0 && ev[0].udata == (void *)0x1234 ? "kept" : "lost");
    {
        char b[8];
        read(pfd[0], b, 5);
    }
    n = kevent(kq, 0, 0, ev, 4, &ts);
    printf("kevent timer: %d event(s), filter %d, udata %s\n", n, n > 0 ? ev[0].filter : 0,
           n > 0 && ev[0].udata == (void *)0x5678 ? "kept" : "lost");
    /* child exit */
    p = fork();
    if (p == 0) { usleep(20000); _exit(1); }
    EV_SET(&ch[0], p, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, 0);
    n = kevent(kq, ch, 1, ev, 4, &ts);
    printf("kevent proc: %d event(s), exit note %s\n", n, n > 0 && (ev[0].fflags & NOTE_EXIT) ? "yes" : "no");
    wait_code(p);
    /* signal (ignored, still counted) */
    signal(SIGUSR1, SIG_IGN);
    EV_SET(&ch[0], SIGUSR1, EVFILT_SIGNAL, EV_ADD, 0, 0, 0);
    kevent(kq, ch, 1, 0, 0, 0);
    kill(getpid(), SIGUSR1);
    kill(getpid(), SIGUSR1);
    n = kevent(kq, 0, 0, ev, 4, &ts);
    printf("kevent signal: %d event(s), count %d\n", n, n > 0 ? (int)ev[0].data : 0);
    signal(SIGUSR1, SIG_DFL);
    EV_SET(&ch[0], 999, EVFILT_READ, EV_ADD, 0, 0, 0);
    n = kevent(kq, ch, 1, 0, 0, 0);
    printf("kevent bad fd: %d %s\n", n, n < 0 ? strerror(errno) : "");
    close(pfd[0]);
    close(pfd[1]);
    close(kq);
}

static void t_locks(void)
{
    int fd;
    struct flock l;
    pid_t p;
    name("lock");
    fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0644);
    write(fd, "0123456789", 10);
    memset(&l, 0, sizeof l);
    l.l_type = F_WRLCK; l.l_whence = SEEK_SET; l.l_start = 2; l.l_len = 5;
    printf("lock: %d\n", fcntl(fd, F_SETLK, &l));
    p = fork();
    if (p == 0) {
        int f2 = open(tmp, O_RDWR);
        struct flock q;
        memset(&q, 0, sizeof q);
        q.l_type = F_WRLCK; q.l_whence = SEEK_SET; q.l_start = 0; q.l_len = 0;
        fcntl(f2, F_GETLK, &q);
        printf("child sees: type %s, start %d, len %d, owner %s\n", q.l_type == F_WRLCK ? "write" : "other",
               (int)q.l_start, (int)q.l_len, q.l_pid == getppid() ? "parent" : "other");
        q.l_type = F_WRLCK; q.l_whence = SEEK_SET; q.l_start = 3; q.l_len = 1;
        printf("child try: %s\n", fcntl(f2, F_SETLK, &q) < 0 ? strerror(errno) : "got it");
        q.l_start = 3; q.l_len = 1;
        printf("child wait: %s\n", fcntl(f2, F_SETLKW, &q) < 0 ? strerror(errno) : "got it after the parent let go");
        _exit(0);
    }
    usleep(200000);
    l.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &l);
    printf("lock child exit %d\n", wait_code(p));
    close(fd);
    unlink(tmp);
}

static void t_sem(void)
{
    sem_t *s;
    char sn[64];
    snprintf(sn, sizeof sn, "/rr_sem_%d", (int)getpid());
    s = sem_open(sn, O_CREAT | O_EXCL, 0600, 1);
    if (s == SEM_FAILED) { printf("sem_open: %s\n", strerror(errno)); return; }
    printf("sem_wait: %d\n", sem_wait(s));
    printf("sem_trywait: %s\n", sem_trywait(s) < 0 ? strerror(errno) : "got it");
    printf("sem_post: %d\n", sem_post(s));
    printf("sem_trywait again: %s\n", sem_trywait(s) < 0 ? strerror(errno) : "got it");
    printf("sem_close: %d, sem_unlink: %d\n", sem_close(s), sem_unlink(sn));
}

static void t_files(void)
{
    struct stat st;
    struct timeval tv[2];
    int fd;
    pid_t p;
    char buf[32];
    ssize_t n;
    name("fifo");
    printf("mkfifo: %d\n", mkfifo(tmp, 0600));
    p = fork();
    if (p == 0) {
        int w = open(tmp, O_WRONLY);
        write(w, "through fifo", 12);
        _exit(0);
    }
    fd = open(tmp, O_RDONLY);
    n = read(fd, buf, sizeof buf - 1);
    buf[n > 0 ? n : 0] = 0;
    close(fd);
    wait_code(p);
    stat(tmp, &st);
    printf("fifo: \"%s\", is fifo %d\n", buf, S_ISFIFO(st.st_mode));
    unlink(tmp);
    name("file");
    fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0644);
    memset(buf, 'x', sizeof buf);
    write(fd, buf, 30);
    printf("truncate: %d", truncate(tmp, 10));
    fstat(fd, &st);
    printf(", size %d", (int)st.st_size);
    ftruncate(fd, 4);
    fstat(fd, &st);
    printf(", ftruncate size %d\n", (int)st.st_size);
    tv[0].tv_sec = 1000000; tv[0].tv_usec = 0; tv[1].tv_sec = 2000000; tv[1].tv_usec = 0;
    printf("futimes: %d", futimes(fd, tv));
    fstat(fd, &st);
    printf(", atime %ld, mtime %ld\n", (long)st.st_atime, (long)st.st_mtime);
    printf("fchown: %d, chown: %d\n", fchown(fd, getuid(), getgid()), chown(tmp, (uid_t)-1, getgid()));
    printf("chflags: %d", chflags(tmp, UF_NODUMP));
    stat(tmp, &st);
    printf(", nodump %d", (st.st_flags & UF_NODUMP) != 0);
    fchflags(fd, 0);
    fstat(fd, &st);
    printf(", fchflags cleared %d\n", (st.st_flags & UF_NODUMP) == 0);
    {   /* setattrlist: modification time */
        struct attrlist al;
        struct timespec t = { 3000000, 0 };
        memset(&al, 0, sizeof al);
        al.bitmapcount = ATTR_BIT_MAP_COUNT;
        al.commonattr = ATTR_CMN_MODTIME;
        printf("setattrlist: %d", setattrlist(tmp, &al, &t, sizeof t, 0));
        stat(tmp, &st);
        printf(", mtime %ld\n", (long)st.st_mtime);
    }
    {
        char ln[300];
        snprintf(ln, sizeof ln, "%s.link", tmp);
        symlink(tmp, ln);
        printf("lchown: %d\n", lchown(ln, (uid_t)-1, getgid()));
        unlink(ln);
    }
    close(fd);
    unlink(tmp);
    {
        static char page[8192] __attribute__((aligned(4096)));
        char vec[2];
        page[0] = 1;
        printf("mlock: %d, munlock: %d, mincore: %d, minherit: %d\n", mlock(page, 4096), munlock(page, 4096),
               mincore(page, 4096, vec), minherit(page, 4096, VM_INHERIT_COPY));
    }
}

static volatile int g_handled;
static void on_usr1(int sig) { (void)sig; g_handled++; }

static void t_signals(void)
{
    sigset_t m, pend, old;
    int sig = 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, 0);
    sigemptyset(&m);
    sigaddset(&m, SIGUSR1);
    sigaddset(&m, SIGUSR2);
    sigprocmask(SIG_BLOCK, &m, &old);
    kill(getpid(), SIGUSR1);
    kill(getpid(), SIGUSR2);                 /* default action terminate, but blocked */
    sigpending(&pend);
    printf("sigpending: usr1 %d, usr2 %d, handled %d\n", sigismember(&pend, SIGUSR1), sigismember(&pend, SIGUSR2), g_handled);
    sigemptyset(&m);
    sigaddset(&m, SIGUSR2);
    printf("sigwait usr2: %d", sigwait(&m, &sig));
    printf(", signal %s\n", sig == SIGUSR2 ? "usr2" : "other");
    sigemptyset(&m);
    sigaddset(&m, SIGUSR1);
    printf("sigwait usr1: %d", sigwait(&m, &sig));
    printf(", signal %s, handler ran %d\n", sig == SIGUSR1 ? "usr1" : "other", g_handled);
    sigpending(&pend);
    printf("nothing pending: %d\n", !sigismember(&pend, SIGUSR1) && !sigismember(&pend, SIGUSR2));
    kill(getpid(), SIGUSR1);
    sigprocmask(SIG_SETMASK, &old, 0);
    printf("unblocked: handled %d\n", g_handled);
    sigemptyset(&m);
    sigaddset(&m, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &m, 0);
    sigprocmask(SIG_BLOCK, 0, &pend);
    printf("pthread_sigmask: blocked %d\n", sigismember(&pend, SIGUSR1));
    pthread_sigmask(SIG_UNBLOCK, &m, 0);
    signal(SIGUSR1, SIG_DFL);
    {   /* a child that blocks a default-terminating signal survives it until it unblocks it */
        pid_t p = fork();
        if (p == 0) {
            sigset_t b;
            sigemptyset(&b);
            sigaddset(&b, SIGTERM);
            sigprocmask(SIG_BLOCK, &b, 0);
            kill(getpid(), SIGTERM);
            printf("child: still alive with SIGTERM blocked\n");
            sigprocmask(SIG_UNBLOCK, &b, 0);
            printf("child: not reached\n");
            _exit(0);
        }
        printf("child result: %d\n", wait_code(p));
    }
}

static void *waiter(void *arg)
{
    sigset_t m;
    int sig = 0;
    (void)arg;
    sigemptyset(&m);
    sigaddset(&m, SIGUSR2);
    sigwait(&m, &sig);
    printf("thread: sigwait returned %s\n", sig == SIGUSR2 ? "usr2" : "other");
    return 0;
}

static void t_thread_signals(void)
{
    pthread_t t;
    sigset_t m, old;
    struct rusage r0, r1;
    long used;
    sigemptyset(&m);
    sigaddset(&m, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &m, &old);
    pthread_create(&t, 0, waiter, 0);
    getrusage(RUSAGE_SELF, &r0);
    usleep(300000);                          /* the waiting thread must not use the processor meanwhile */
    getrusage(RUSAGE_SELF, &r1);
    used = (r1.ru_utime.tv_sec - r0.ru_utime.tv_sec + r1.ru_stime.tv_sec - r0.ru_stime.tv_sec) * 1000000L +
           r1.ru_utime.tv_usec - r0.ru_utime.tv_usec + r1.ru_stime.tv_usec - r0.ru_stime.tv_usec;
    kill(getpid(), SIGUSR2);
    pthread_join(t, 0);
    pthread_sigmask(SIG_SETMASK, &old, 0);
    printf("threads: waiting used little time %d\n", used < 60000);
}

static void t_process(void)
{
    pid_t p = fork();
    if (p == 0) {
        pid_t s = setsid();
        printf("setsid: %s, getsid %s, getpgid %s\n", s == getpid() ? "new session" : strerror(errno),
               getsid(0) == getpid() ? "right" : "wrong", getpgid(0) == getpid() ? "right" : "wrong");
        printf("setpgid: %d\n", setpgid(0, 0));
        _exit(0);
    }
    wait_code(p);
    errno = 0;
    printf("getpriority: %d (errno %d)", getpriority(PRIO_PROCESS, 0), errno);
    printf(", setpriority: %d", setpriority(PRIO_PROCESS, 0, 5));
    printf(", now %d\n", getpriority(PRIO_PROCESS, 0));
    printf("ptrace deny attach: %d\n", ptrace(PT_DENY_ATTACH, 0, 0, 0));
}

static void t_tty(void)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY), s, fl = 1, r;
    struct termios t;
    struct winsize w = { 24, 80, 0, 0 }, w2;
    char *sn, buf[16];
    if (m < 0) { printf("posix_openpt: %s\n", strerror(errno)); return; }
    grantpt(m);
    unlockpt(m);
    sn = ptsname(m);
    s = sn ? open(sn, O_RDWR | O_NOCTTY) : -1;
    if (s < 0) { printf("pty: %s\n", strerror(errno)); close(m); return; }
    tcgetattr(s, &t);
    t.c_lflag &= ~(ECHO | ICANON);
    t.c_cc[VMIN] = 1;
    printf("tcsetattr: %d", tcsetattr(s, TCSANOW, &t));
    memset(&t, 0, sizeof t);
    tcgetattr(s, &t);
    printf(", echo %d, icanon %d, vmin %d\n", (t.c_lflag & ECHO) != 0, (t.c_lflag & ICANON) != 0, t.c_cc[VMIN]);
    printf("TIOCSWINSZ: %d", ioctl(m, TIOCSWINSZ, &w));
    memset(&w2, 0, sizeof w2);
    ioctl(s, TIOCGWINSZ, &w2);
    printf(", rows %d, cols %d\n", w2.ws_row, w2.ws_col);
    printf("FIONBIO: %d", ioctl(s, FIONBIO, &fl));
    r = (int)read(s, buf, sizeof buf);
    printf(", empty read: %s\n", r < 0 ? strerror(errno) : "data");
    close(s);
    close(m);
}

static void t_sysctl(void)
{
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() }, k;
    struct kinfo_proc kp;
    size_t len = sizeof kp;
    struct loadavg la;
    struct xsw_usage sw;
    struct clockinfo ci;
    struct ifaddrs *ifa, *i;
    struct utmpx *u;
    char args[8192];
    memset(&kp, 0, sizeof kp);
    if (sysctl(mib, 4, &kp, &len, 0, 0) < 0) printf("KERN_PROC: %s\n", strerror(errno));
    else printf("KERN_PROC: size %d, pid %s, ppid %s, uid %s, ruid %s, pgid %s, name %s, traced %d, state %d\n", (int)len,
                kp.kp_proc.p_pid == getpid() ? "right" : "wrong", kp.kp_eproc.e_ppid == getppid() ? "right" : "wrong",
                kp.kp_eproc.e_ucred.cr_uid == getuid() ? "right" : "wrong",
                kp.kp_eproc.e_pcred.p_ruid == getuid() ? "right" : "wrong", kp.kp_eproc.e_pgid == getpgrp() ? "right" : "wrong",
                strncmp(kp.kp_proc.p_comm, "syscalls", 8) ? kp.kp_proc.p_comm : "this program",
                (kp.kp_proc.p_flag & P_TRACED) != 0, kp.kp_proc.p_stat);
    mib[1] = KERN_PROCARGS2; mib[2] = getpid();
    len = sizeof args;
    if (sysctl(mib, 3, args, &len, 0, 0) < 0) printf("KERN_PROCARGS2: %s\n", strerror(errno));
    else {
        int argc;
        char *p = args + 4;
        memcpy(&argc, args, 4);
        printf("KERN_PROCARGS2: argc %d, path %s", argc, strstr(p, "syscalls") ? "this program" : p);
        p += strlen(p);
        while (p < args + len && !*p) p++;
        printf(", argv[0] %s\n", strstr(p, "syscalls") ? "this program" : p);
    }
    {   /* size queries: no buffer, the kernel reports the length (getifaddrs, ps, sysctlbyname with strings) */
        int all[3] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL };
        size_t need = 12345, need2 = 0;
        int r1 = sysctl(all, 3, 0, &need, 0, 0);
        int r2 = sysctlbyname("kern.ostype", 0, &need2, 0, 0);
        printf("size query: KERN_PROC_ALL %d, whole entries %d, plausible %d; kern.ostype %d, length %d\n", r1,
               need % sizeof(struct kinfo_proc) == 0, need > 10 * sizeof(struct kinfo_proc) && need != 12345, r2, (int)need2);
    }
    mib[0] = CTL_VM; mib[1] = VM_LOADAVG; len = sizeof la;
    sysctl(mib, 2, &la, &len, 0, 0);
    printf("loadavg: fscale %ld, sane %d\n", la.fscale, la.ldavg[0] < 1000u * (unsigned)la.fscale && la.ldavg[2] < 1000u * (unsigned)la.fscale);
    mib[1] = VM_SWAPUSAGE; len = sizeof sw;
    sysctl(mib, 2, &sw, &len, 0, 0);
    printf("swapusage: page size %u, used <= total %d\n", sw.xsu_pagesize, sw.xsu_used <= sw.xsu_total);
    mib[0] = CTL_KERN; mib[1] = KERN_CLOCKRATE; len = sizeof ci;
    sysctl(mib, 2, &ci, &len, 0, 0);
    printf("clockrate: hz %d, tick %d\n", ci.hz, ci.tick);
    {   /* struct vfsconf (getvfsbyname, lsvfs) and struct nfsstats (nfsstat: type number from getvfsbyname) */
        struct vfsconf v;
        int nm[3] = { CTL_VFS, 0, 1 }, st[200];
        if (getvfsbyname("hfs", &v) < 0) printf("getvfsbyname hfs: %s\n", strerror(errno));
        else printf("getvfsbyname hfs: name %s, typenum %d, mounted %d, local %d\n", v.vfc_name, v.vfc_typenum,
                    v.vfc_refcount > 0 && v.vfc_refcount < 1000, (v.vfc_flags & MNT_LOCAL) != 0);
        if (getvfsbyname("nfs", &v) < 0) printf("getvfsbyname nfs: %s\n", strerror(errno));
        else {
            nm[1] = v.vfc_typenum;
            len = sizeof st;
            memset(st, 0, sizeof st);
            if (sysctl(nm, 3, st, &len, 0, 0) < 0) printf("nfsstats: %s\n", strerror(errno));
            else printf("nfsstats: size %d, counters plausible %d\n", (int)len, st[0] >= 0 && st[0] < 100000000 && st[16] >= 0 && st[16] < 100000000);
        }
    }
    if (getifaddrs(&ifa) == 0) {
        for (i = ifa, k = 0; i && k < 24; i = i->ifa_next, k++) {
            int fam = i->ifa_addr ? i->ifa_addr->sa_family : -1;
            printf("ifaddr %s: family %d, flags %x", i->ifa_name, fam, i->ifa_flags);
            if (fam == AF_LINK) {
                struct sockaddr_dl *d = (struct sockaddr_dl *)i->ifa_addr;
                printf(", index %d, type %d, name length %d, address length %d", d->sdl_index, d->sdl_type, d->sdl_nlen, d->sdl_alen);
            } else if (fam == AF_INET) {
                printf(", %s", inet_ntoa(((struct sockaddr_in *)i->ifa_addr)->sin_addr));
                if (i->ifa_netmask) printf("/%s", inet_ntoa(((struct sockaddr_in *)i->ifa_netmask)->sin_addr));
            } else if (fam == AF_INET6) {
                printf(", scope %u", ((struct sockaddr_in6 *)i->ifa_addr)->sin6_scope_id);
            }
            printf("\n");
        }
        freeifaddrs(ifa);
    }
    printf("if_nametoindex lo0: %u\n", if_nametoindex("lo0"));
    setutxent();
    for (k = 0; (u = getutxent()) != 0; )
        if (u->ut_type == USER_PROCESS) {
            if (!k++) printf("utmpx: first user on %s, pid %s, time %s\n", u->ut_line, u->ut_pid > 0 ? "set" : "missing",
                             u->ut_tv.tv_sec > 1000000000 ? "plausible" : "wrong");
        }
    endutxent();
    printf("utmpx: %d user entries\n", k);
    {   /* libproc (proc_info): process list, own process, a descriptor's path */
        int pids[4096], n, fd;
        struct proc_bsdinfo bi;
        struct proc_taskinfo ti;
        struct vnode_fdinfowithpath vp;
        char path[PROC_PIDPATHINFO_MAXSIZE];
        n = proc_listpids(PROC_ALL_PIDS, 0, pids, sizeof pids);
        for (k = 0; k < n / (int)sizeof(int) && pids[k] != getpid(); k++) ;
        printf("proc_listpids: plausible %d, own pid in it %d\n", n > 10 * (int)sizeof(int), k < n / (int)sizeof(int));
        memset(&bi, 0, sizeof bi);
        n = proc_pidinfo(getpid(), PROC_PIDTBSDINFO, 0, &bi, sizeof bi);
        printf("PROC_PIDTBSDINFO: size %d, pid %s, ppid %s, uid %s, name %s, started %s\n", n, bi.pbi_pid == (uint32_t)getpid() ? "right" : "wrong",
               bi.pbi_ppid == (uint32_t)getppid() ? "right" : "wrong", bi.pbi_uid == getuid() ? "right" : "wrong",
               strncmp(bi.pbi_comm, "syscalls", 8) ? bi.pbi_comm : "this program", bi.pbi_start.tv_sec > 1000000000 ? "plausible" : "wrong");
        n = proc_pidinfo(getpid(), PROC_PIDTASKINFO, 0, &ti, sizeof ti);
        printf("PROC_PIDTASKINFO: size %d, threads %s, resident size plausible %d\n", n, ti.pti_threadnum >= 1 && ti.pti_threadnum < 100 ? "plausible" : "wrong",
               ti.pti_resident_size > 100000 && ti.pti_resident_size < 2000000000ull);
        n = proc_pidpath(getpid(), path, sizeof path);
        printf("proc_pidpath: %s\n", n > 0 && strstr(path, "syscalls") ? "this program" : path);
        snprintf(tmp, sizeof tmp, "/tmp/syscalls_proc.%d", (int)getpid());
        fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0600);
        write(fd, "12345", 5);
        n = proc_pidfdinfo(getpid(), fd, PROC_PIDFDVNODEPATHINFO, &vp, sizeof vp);
        printf("PROC_PIDFDVNODEPATHINFO: size %d, path %s, size %lld, offset %lld, mode %o\n", n,
               strstr(vp.pvip.vip_path, "syscalls_proc") ? "right" : vp.pvip.vip_path, (long long)vp.pvip.vip_vi.vi_stat.vst_size,
               (long long)vp.pfi.fi_offset, vp.pvip.vip_vi.vi_stat.vst_mode & 07777);
        close(fd);
        unlink(tmp);
    }
}

/* System V shared memory (SheepShaver, X11's MIT-SHM): create, attach, write, read the segment's state, detach, remove */
static void t_sysv_shm(void)
{
    struct shmid_ds ds;
    int id = shmget(IPC_PRIVATE, 8192, IPC_CREAT | 0600);
    char *p;
    if (id < 0) { printf("shmget failed\n"); return; }
    p = shmat(id, 0, 0);
    if (p == (char *)-1) { printf("shmat failed\n"); shmctl(id, IPC_RMID, 0); return; }
    strcpy(p, "shared");
    memset(&ds, 0, sizeof ds);
    shmctl(id, IPC_STAT, &ds);
    printf("shm: %s, size %u, attached %u, mode %o\n", p, (unsigned)ds.shm_segsz, (unsigned)ds.shm_nattch, ds.shm_perm.mode & 0777);
    ds.shm_perm.mode = 0640;
    printf("shm: set %d", shmctl(id, IPC_SET, &ds));
    memset(&ds, 0, sizeof ds);
    shmctl(id, IPC_STAT, &ds);
    printf(", mode now %o, detach %d, remove %d\n", ds.shm_perm.mode & 0777, shmdt(p), shmctl(id, IPC_RMID, 0));
}

/* the main thread's stack as pthread reports it (from sysctl KERN_USRSTACK): conservative garbage collectors scan from
   the stack pointer up to its top (JavaScriptCore) */
static void t_stack(void)
{
    char here;
    char *top = pthread_get_stackaddr_np(pthread_self());
    size_t size = pthread_get_stacksize_np(pthread_self());
    long d = top - &here;
    printf("main stack: top above the stack pointer %s, within the stack size %s\n", d > 0 ? "yes" : "no",
           d > 0 && (size_t)d < size ? "yes" : "no");
}

/* getattrlist asking for directory and file attributes at once: the kernel packs only the group that fits the object,
   the name comes after it (LaunchServices asks so for bundles; a swapped name made it crash) */
static void attrs_show(const char *what, const char *path, int objtype)
{
    struct attrlist al;
    uint32_t buf[64], *p;
    attrreference_t *nr;
    memset(&al, 0, sizeof al);
    memset(buf, 0, sizeof buf);
    al.bitmapcount = ATTR_BIT_MAP_COUNT;
    al.commonattr = ATTR_CMN_NAME | (objtype ? ATTR_CMN_OBJTYPE : 0);
    al.dirattr = ATTR_DIR_ENTRYCOUNT;
    al.fileattr = ATTR_FILE_TOTALSIZE | ATTR_FILE_DATALENGTH;
    if (getattrlist(path, &al, buf, sizeof buf, 0) != 0) { printf("getattrlist %s: error %d\n", what, errno); return; }
    nr = (attrreference_t *)(buf + 1);
    p = buf + 3;
    {                                       /* the name is that of the path (it contains the pid: not printed) */
        const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
        int same = nr->attr_length == strlen(base) + 1 && !memcmp((char *)nr + nr->attr_dataoffset, base, strlen(base));
        printf("getattrlist %s: name %s", what, same ? "right" : "wrong");
    }
    if (objtype) printf(", type %u", *p++);
    if (strstr(what, "directory")) printf(", entries %u\n", *p);
    else printf(", size %lld, data %lld\n", *(long long *)p, *(long long *)(p + 2));
}

static void t_dir_attrs(void)
{
    char file[200];
    int fd;
    name("dir");
    mkdir(tmp, 0700);
    snprintf(file, sizeof file, "%s/file.rtf", tmp);
    fd = open(file, O_RDWR | O_CREAT | O_TRUNC, 0600);
    write(fd, "{\\rtf1}", 7);
    close(fd);
    attrs_show("directory with type", tmp, 1);
    attrs_show("directory without type", tmp, 0);
    attrs_show("file with type", file, 1);
    attrs_show("file without type", file, 0);
    unlink(file);
    rmdir(tmp);
}

/* libquarantine (__mac_syscall of policy "mb"): LaunchServices asks for the quarantine of every document it opens */
extern void *_qtn_file_alloc(void);
extern void _qtn_file_free(void *);
extern int _qtn_file_init_with_path(void *, const char *);
extern const char *_qtn_file_get_identifier(void *);
static void t_quarantine(void)
{
    static const char label[] = "0000;4b000000;Safari;|com.apple.Safari";
    void *q = _qtn_file_alloc();
    int fd, r;
    name("qtn");
    fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0600);
    close(fd);
    printf("quarantine of a new file: %d\n", _qtn_file_init_with_path(q, tmp));
    setxattr(tmp, "com.apple.quarantine", label, sizeof label - 1, 0, 0);
    r = _qtn_file_init_with_path(q, tmp);
    printf("quarantine after setxattr: %d, agent %s\n", r, r == 0 ? _qtn_file_get_identifier(q) : "-");
    _qtn_file_free(q);
    unlink(tmp);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, 0, _IONBF, 0);
    if (argc > 1 && !strcmp(argv[1], "exec-child")) return exec_child(argv);
    self = argv[0];
    t_fork();
    t_exec();
    t_spawn();
    t_waitid();
    t_kqueue();
    t_locks();
    t_sem();
    t_files();
    t_signals();
    t_tty();
    t_sysctl();
    t_thread_signals();
    t_process();
    t_stack();
    t_sysv_shm();
    t_dir_attrs();
    t_quarantine();
    return 0;
}
