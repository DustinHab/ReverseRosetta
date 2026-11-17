/* main.c - rosetta: run Intel programs (i386) on PowerPC.
 *
 *   rosetta [-s] [-0 name] program [arguments ...]
 *     -s       trace: every executed instruction to stderr
 *     -0 name  argv[0] of the program (execve of an Intel program passes the caller's argv[0] this way)
 *
 * Stack as with the kernel: (for dyld first the address of the Mach header), argc, argv..., 0, envp..., 0,
 * apple (program path), 0, then the strings.
 */
#include "cpu.h"
#include "interp.h"
#include "macho.h"
#include "commpage.h"
#include "mach_msg.h"
#include "jit.h"
#include "sys.h"
#include "thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <mach/mach.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>

extern char **environ;

#define STACK_SIZE (8u << 20)
uint32_t g_stack_top;
#define DYLD_TARGET 0x0fe00000u           /* the i386 dyld wants to go to 0x8fe00000, the host's dyld is there */

static uint32_t chain_put(uint32_t *top, const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1;
    *top -= n;
    memcpy((void *)(uintptr_t)*top, s, n);
    return *top;
}

static uint32_t stack_build(int argc, char **argv, const char *path, uint32_t mh, int with_mh)
{
    void *s = mmap(0, STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    uint32_t top, *ptr, nenv = 0, i, n, sp;
    uint32_t *argvp, *envp, apple;
    if (s == MAP_FAILED) { perror("rosetta: stack"); exit(1); }
    top = (uint32_t)(uintptr_t)s + STACK_SIZE;
    g_stack_top = top;                      /* libSystem asks for it (sysctl KERN_USRSTACK): the main thread's stack */
    while (environ[nenv]) nenv++;
    argvp = calloc((size_t)argc + 1, 4);
    envp = calloc(nenv + 1, 4);
    apple = chain_put(&top, path);
    for (i = 0; i < nenv; i++) {
        /* "_" (the shell's last command) names rosetta: for the guest it is the program, as natively */
        const char *s = environ[i], *b = strrchr(s, '/');
        char fixed[1100];
        if (!strncmp(s, "_=", 2) && !strcmp(b ? b + 1 : s + 2, "rosetta")) { snprintf(fixed, sizeof fixed, "_=%s", path); s = fixed; }
        envp[i] = chain_put(&top, s);
    }
    for (i = 0; i < (uint32_t)argc; i++) argvp[i] = chain_put(&top, argv[i]);
    top &= ~15u;
    n = (with_mh ? 1 : 0) + 1 + (uint32_t)argc + 1 + nenv + 1 + 2;
    sp = (top - n * 4) & ~15u;
    ptr = (uint32_t *)(uintptr_t)sp;
    i = 0;
    if (with_mh) st32((uint32_t)(uintptr_t)&ptr[i++], mh);
    st32((uint32_t)(uintptr_t)&ptr[i++], (uint32_t)argc);
    for (n = 0; n < (uint32_t)argc; n++) st32((uint32_t)(uintptr_t)&ptr[i++], argvp[n]);
    st32((uint32_t)(uintptr_t)&ptr[i++], 0);
    for (n = 0; n < nenv; n++) st32((uint32_t)(uintptr_t)&ptr[i++], envp[n]);
    st32((uint32_t)(uintptr_t)&ptr[i++], 0);
    st32((uint32_t)(uintptr_t)&ptr[i++], apple);
    st32((uint32_t)(uintptr_t)&ptr[i++], 0);
    free(argvp);
    free(envp);
    return sp;
}

static Cpu cpu __attribute__((aligned(16)));    /* main thread (secondary threads: thread.c) */
static const char *g_prog_path;
static int g_error_start, g_error_load;     /* page faults until main or until the guest is loaded (ROSETTA_STATS) */
static struct timeval g_t0;
static int g_only_interp;
static const char *g_profile;

/* memory fault in the guest (or in the translator): show the guest state */
/* hex number without stdio (in the fault handler: stdio can be deadlocked or broken itself) */
static char *hex_put(char *p, const char *text, uint32_t v)
{
    int i;
    while (*text) *p++ = *text++;
    for (i = 28; i >= 0; i -= 4) *p++ = "0123456789abcdef"[(v >> i) & 15];
    return p;
}

/* Crash report: the full state (guest registers, call chain, memory calls, host location) goes to
   ~/Library/Logs/rosetta/PROGRAM-PID.txt while stderr points there; afterwards stderr gets one line with the path, as a
   native program that crashes prints nothing of its own. ROSETTA_CRASH_STDERR=1: everything on stderr */
static int g_crash_saved = -1;
static char g_crash_path[1024];

void main_crash_begin(void)
{
    const char *home = getenv("HOME"), *name;
    int fd;
    if (g_crash_saved >= 0 || getenv("ROSETTA_CRASH_STDERR") || !home || !*home || !g_prog_path) return;
    name = strrchr(g_prog_path, '/') ? strrchr(g_prog_path, '/') + 1 : g_prog_path;
    snprintf(g_crash_path, sizeof g_crash_path, "%s/Library/Logs/rosetta", home);
    mkdir(g_crash_path, 0755);
    snprintf(g_crash_path, sizeof g_crash_path, "%s/Library/Logs/rosetta/%s-%d.txt", home, name, (int)getpid());
    if ((fd = open(g_crash_path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) return;
    fflush(stderr);
    g_crash_saved = dup(2);
    if (g_crash_saved < 0) { close(fd); return; }
    dup2(fd, 2);
    close(fd);
}

void main_crash_end(const char *what)
{
    char line[1400];
    int n;
    if (g_crash_saved < 0) return;
    fflush(stderr);
    dup2(g_crash_saved, 2);
    close(g_crash_saved);
    g_crash_saved = -1;
    n = snprintf(line, sizeof line, "rosetta: %s; report in %s\n", what, g_crash_path);
    if (n > 0) write(2, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
}

/* end like a native process that did not handle an exception: terminated by the signal (the parent's wait sees
   WIFSIGNALED, the shell prints "Segmentation fault" etc.); falls back to exit status 128 + sig */
static void end_by_signal(int sig)
{
    sigset_t m;
    fflush(stdout);
    signal(sig, SIG_DFL);
    sigemptyset(&m);
    sigaddset(&m, sig);
    sigprocmask(SIG_UNBLOCK, &m, 0);
    kill(getpid(), sig);
    _exit(128 + sig);
}

static void memfault(int sig, siginfo_t *info, void *k)
{
    char t[64];
    if (jit_prefetch_error(k)) return;      /* prefetched past the end of an array: the value is never needed */
    if ((sig == SIGSEGV || sig == SIGBUS) && jit_probe_fault()) return;   /* (jumps back into safe_read) */
    if (sig == SIGSEGV || sig == SIGBUS) {
        /* a fault of guest code (translated or interpreted, not the translator's own) with a handler of the guest:
           back to this thread's dispatcher, which delivers the signal (sys_fault, jit_fault_state) */
        Cpu *c = g_thread_cur ? g_thread_cur : &cpu;
        int exact;
        /* first: a write to a JIT code page the translator protected (jit.c, smc) */
        if (jit_smc_fault((uint32_t)(uintptr_t)info->si_addr, k, c->host_thread == (void *)pthread_self() ? c : 0)) return;
        if (c->fault_jmp && c->host_thread == (void *)pthread_self() && (exact = jit_fault_state(c, k)) >= 0) {
            /* translated code: the handler runs nested, then the faulting instruction again (sys.c) */
            if (!exact && sys_fault_nest(c, sig, info->si_code, (uint32_t)(uintptr_t)info->si_addr, k)) return;
            if (sys_fault(c, sig, info->si_code, (uint32_t)(uintptr_t)info->si_addr, exact))
                _longjmp(*(jmp_buf *)c->fault_jmp, 1);
        }
    }
    main_crash_begin();
    {   /* first without stdio: signal, address, srr0, lr */
        ucontext_t *u = (ucontext_t *)k;
        char b[128], *p = b;
#if __DARWIN_UNIX03
        uint32_t pc = (uint32_t)u->uc_mcontext->__ss.__srr0, lr = (uint32_t)u->uc_mcontext->__ss.__lr;
#else
        uint32_t pc = (uint32_t)u->uc_mcontext->ss.srr0, lr = (uint32_t)u->uc_mcontext->ss.lr;
#endif
        p = hex_put(p, "rosetta: fault, signal ", (uint32_t)sig);
        p = hex_put(p, " address ", (uint32_t)(uintptr_t)info->si_addr);
        p = hex_put(p, " srr0 ", pc);
        p = hex_put(p, " lr ", lr);
        *p++ = '\n';
        write(2, b, (size_t)(p - b));
        sys_mem_show((uint32_t)(uintptr_t)info->si_addr);
    }
    {   /* stop the sampling clock at once: if the crash reporter stops the process, the clock callbacks pile up in the
           kernel, each gets its own thread with a kernel stack until "kernel map entries" overflows (panic, 14 Nov) */
        struct itimerval stop;
        memset(&stop, 0, sizeof stop);
        setitimer(ITIMER_REAL, &stop, 0);
        setitimer(ITIMER_PROF, &stop, 0);
        signal(SIGALRM, SIG_IGN);
        signal(SIGPROF, SIG_IGN);
    }
    {                                       /* host location (debugging the generated code) */
        ucontext_t *u = (ucontext_t *)k;
        uint32_t r1, *frame;
        int depth;
#if __DARWIN_UNIX03
        fprintf(stderr, "rosetta: host srr0 %08x lr %08x ctr %08x\n", (unsigned)u->uc_mcontext->__ss.__srr0,
                (unsigned)u->uc_mcontext->__ss.__lr, (unsigned)u->uc_mcontext->__ss.__ctr);
        r1 = u->uc_mcontext->__ss.__r1;
#else
        fprintf(stderr, "rosetta: host srr0 %08x lr %08x ctr %08x\n", (unsigned)u->uc_mcontext->ss.srr0,
                (unsigned)u->uc_mcontext->ss.lr, (unsigned)u->uc_mcontext->ss.ctr);
        r1 = u->uc_mcontext->ss.r1;
#endif
        fprintf(stderr, "rosetta: call chain:");     /* frame chain: *(r1) = previous frame, 8(previous) = LR */
        for (depth = 0, frame = (uint32_t *)(uintptr_t)r1; depth < 6 && frame && ((uintptr_t)frame & 3) == 0; depth++) {
            frame = (uint32_t *)(uintptr_t)frame[0];
            if (!frame) break;
            fprintf(stderr, " %08x", frame[2]);
        }
        fprintf(stderr, "\n");
        jit_crashdump(k);
    }
    snprintf(t, sizeof t, "memory fault (signal %d) at address %p", sig, info->si_addr);
    {
        Cpu *c = g_thread_cur ? g_thread_cur : &cpu;  /* thread currently executing guest code */
        /* if eip itself points to unmapped memory, the display must not read the instruction there (otherwise endless
   loop) */
        if ((uint32_t)(uintptr_t)info->si_addr >= c->eip && (uint32_t)(uintptr_t)info->si_addr < c->eip + 16) {
            fprintf(stderr, "rosetta: %s, jump to unmapped memory\n", t);
            c->eip = 0;
        }
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS, SIG_DFL);
        signal(SIGILL, SIG_DFL);
        state_show(c, t);
        snprintf(t, sizeof t, "memory fault (signal %d) at address %p, eip %08x", sig, info->si_addr, c->eip);
    }
    main_crash_end(t);
    end_by_signal(sig);
}

/* sampling profile (ROSETTA_HPROFILE=file): SIGALRM (ITIMER_REAL, fine resolution) reads srr0, lr and the
   return addresses of the frame chain; at the end one line per sample (hex) into the file */
#define PROFILE_MAX 100000
#define PROFILE_DEPTH 12
static uint32_t *g_prof, *g_prof_eip;  /* per sample: host addresses; guest eip in Cpu (valid when interpreting) */
static volatile int g_prof_n;

static void profile_sample(int sig, siginfo_t *info, void *k)
{
    ucontext_t *u = (ucontext_t *)k;
    uint32_t *z, *frame, r1;
    int t;
    (void)sig; (void)info;
    if (g_prof_n >= PROFILE_MAX) return;
    z = g_prof + PROFILE_DEPTH * g_prof_n;
    memset(z, 0, PROFILE_DEPTH * 4);
#if __DARWIN_UNIX03
    z[0] = (uint32_t)u->uc_mcontext->__ss.__srr0;
    z[1] = (uint32_t)u->uc_mcontext->__ss.__lr;
    r1 = (uint32_t)u->uc_mcontext->__ss.__r1;
#else
    z[0] = (uint32_t)u->uc_mcontext->ss.srr0;
    z[1] = (uint32_t)u->uc_mcontext->ss.lr;
    r1 = (uint32_t)u->uc_mcontext->ss.r1;
#endif
    /* frame chain: *(r1) = the caller's frame, at 8 in it the saved lr of the callee */
    for (t = 2, frame = (uint32_t *)(uintptr_t)r1; t < PROFILE_DEPTH; t++) {
        uint32_t *pre = (uint32_t *)(uintptr_t)frame[0];
        if (!pre || ((uintptr_t)pre & 3) || pre <= frame || (uintptr_t)pre - (uintptr_t)frame > (1u << 20)) break;
        frame = pre;
        z[t] = frame[2];
    }
    g_prof_eip[g_prof_n] = g_thread_cur ? g_thread_cur->eip : cpu.eip;
    g_prof_n++;
}

/* Intel images in guest memory (i386 Mach header at the start of a range): start, end of __TEXT, name */
#define IMAGES_MAX 256
static struct { uint32_t start, end, wish; char name[160]; } g_pimage[IMAGES_MAX];
static int g_npimage;

static void images_find(void)
{
    vm_address_t a = 0x1000;
    vm_size_t sz;
    vm_region_basic_info_data_t info;
    mach_msg_type_number_t n;
    mach_port_t obj;
    g_npimage = 0;
    while (g_npimage < IMAGES_MAX) {
        uint32_t head[2] = { 0, 0 };
        vm_size_t was_read = 0;
        n = VM_REGION_BASIC_INFO_COUNT;
        if (vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO, (vm_region_info_t)&info, &n, &obj)) break;
        if (a >= 0x90000000u) break;        /* above: shared region of the host (reported readable, but not always) */
        if ((info.protection & VM_PROT_READ) && sz >= 4096 &&
            vm_read_overwrite(mach_task_self(), a, 8, (vm_address_t)head, &was_read) == KERN_SUCCESS && was_read == 8 &&
            ld32((uint32_t)(uintptr_t)&head[0]) == 0xfeedface && ld32((uint32_t)(uintptr_t)&head[1]) == 7) {
            uint32_t type = ld32((uint32_t)a + 12), ncmds = ld32((uint32_t)a + 16), lc = (uint32_t)a + 28, k, textsz = 0;
            const char *name = "?";
            for (k = 0; k < ncmds && lc < (uint32_t)a + 0x8000; k++) {
                uint32_t cmd = ld32(lc), cgr = ld32(lc + 4);
                if (cmd == 0xd || cmd == 0xf) name = (const char *)(uintptr_t)(lc + ld32(lc + 8));
                if (cmd == 1 && !strncmp((const char *)(uintptr_t)(lc + 8), "__TEXT", 16)) {
                    textsz = ld32(lc + 28);
                    g_pimage[g_npimage].wish = ld32(lc + 24);
                }
                if (cgr < 8) break;
                lc += cgr;
            }
            if (type == 2 && g_prog_path) name = g_prog_path;
            g_pimage[g_npimage].start = (uint32_t)a;
            g_pimage[g_npimage].end = (uint32_t)a + (textsz ? textsz : (uint32_t)sz);
            snprintf(g_pimage[g_npimage].name, sizeof g_pimage[0].name, "%s", name);
            g_npimage++;
        }
        a += sz;
    }
}

/* guest address as "path+offset" (offset from the Mach header) or "-" */
static void guest_name(FILE *f, uint32_t g)
{
    int k;
    for (k = 0; k < g_npimage; k++)
        if (g - g_pimage[k].start < g_pimage[k].end - g_pimage[k].start) {
            fprintf(f, "%s+%x", g_pimage[k].name, g - g_pimage[k].start);
            return;
        }
    fprintf(f, g ? "%08x" : "-", g);
}

void guest_where(FILE *f, uint32_t g, int refresh)
{
    if (refresh || !g_npimage) images_find();
    guest_name(f, g);
}

static void profile_start(void)
{
    struct sigaction sa;
    struct itimerval it;
    const char *us = getenv("ROSETTA_HPROFILE_US");
    g_prof = malloc(PROFILE_MAX * PROFILE_DEPTH * 4);
    g_prof_eip = malloc(PROFILE_MAX * 4);
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = profile_sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGALRM, &sa, 0);
    sigaction(SIGPROF, &sa, 0);
    it.it_interval.tv_sec = it.it_value.tv_sec = 0;
    /* not below 50 us: at 29 us the kernel created over 5000 threads for the clock callbacks */
    it.it_interval.tv_usec = it.it_value.tv_usec = us && atoi(us) >= 50 ? atoi(us) : us ? 50 : 100;
    /* ROSETTA_HPROFILE_CPU=1: processor time only (threads that wait are not sampled) */
    setitimer(getenv("ROSETTA_HPROFILE_CPU") ? ITIMER_PROF : ITIMER_REAL, &it, 0);
}

static void profile_write(const char *path)
{
    struct itimerval it;
    FILE *f;
    int i, t;
    memset(&it, 0, sizeof it);
    setitimer(ITIMER_REAL, &it, 0);
    setitimer(ITIMER_PROF, &it, 0);
    f = fopen(path, "a");
    if (!f) return;
    for (i = 0; i < g_prof_n; i++) {
        for (t = 0; t < PROFILE_DEPTH && (t < 2 || g_prof[PROFILE_DEPTH * i + t]); t++)
            fprintf(f, t ? " %08x" : "%08x", g_prof[PROFILE_DEPTH * i + t]);
        fputc('\n', f);
    }
    fclose(f);
    {   /* guest side per sample (line by line as above): block in the code cache and guest eip, as image+offset */
        char p2[1024];
        snprintf(p2, sizeof p2, "%s.guest", path);
        if (!(f = fopen(p2, "a"))) return;
        images_find();
        jit_blocks_sort();
        for (i = 0; i < g_prof_n; i++) {
            guest_name(f, jit_guest_eip(g_prof[PROFILE_DEPTH * i]));
            fputc(' ', f);
            guest_name(f, g_prof_eip[i]);
            fputc('\n', f);
        }
        fclose(f);
    }
    {   /* host libraries (shared cache, nm addresses of the files do not match there): name via dladdr, per
           sample srr0 and lr (line by line as above, "-" in rosetta itself or in the code cache) */
        char p3[1024];
        snprintf(p3, sizeof p3, "%s.host", path);
        if (!(f = fopen(p3, "a"))) return;
        for (i = 0; i < g_prof_n; i++) {
            for (t = 0; t < 2; t++) {
                uint32_t a = g_prof[PROFILE_DEPTH * i + t];
                Dl_info di;
                if (a >= 0x90000000u && dladdr((void *)(uintptr_t)a, &di) && di.dli_sname)
                    fprintf(f, t ? " %s" : "%s", di.dli_sname);
                else fputs(t ? " -" : "-", f);
            }
            fputc('\n', f);
        }
        fclose(f);
    }
}
static int end_expiry(Cpu *c, int r);

/* profile of a program that does not end by itself (graphical): SIGTERM writes it and terminates (not
   signal-safe, debugging only) */
static void profile_sigterm(int sig)
{
    (void)sig;
    if (g_profile) profile_write(g_profile);
    _exit(143);
}

int main(int argc, char **argv)
{
    const char *profil = getenv("ROSETTA_HPROFILE"), *name = 0, *path;
    Image prog, dyld;
    int i = 1, r;
    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "-s")) g_lane = 1;
        else if (!strcmp(argv[i], "-0") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else { fprintf(stderr, "rosetta: unknown option %s\n", argv[i]); return 2; }
        i++;
    }
    if (i >= argc) { fprintf(stderr, "usage: rosetta [-s] [-0 name] program [arguments ...]\n"); return 2; }
    path = argv[i];
    if (name) argv[i] = (char *)name;        /* argv[0] of the guest; the file is still loaded from path */
    g_log = getenv("ROSETTA_LOG") ? (atoi(getenv("ROSETTA_LOG")) > 1 ? atoi(getenv("ROSETTA_LOG")) : 1) : 0;   /* 2: with time and thread, 3: also the words of Mach messages */
    sys_init();
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = memfault;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, 0);
        sigaction(SIGBUS, &sa, 0);
        sigaction(SIGILL, &sa, 0);
    }
    g_error_start = getenv("ROSETTA_STATS") ? jit_pagefault() : 0;
    g_prog_path = path;
    sys_set_args(argc - i, argv + i, path);
    if (commpage_create()) return 1;
    if (macho_load(path, 0, &prog)) return 1;
    memset(&cpu, 0, sizeof cpu);
    flags_set(&cpu, F_IF);
    cpu.fpu_ctl = 0x037f;
    cpu.mxcsr = 0x1f80;
    if (prog.has_dyld) {
        if (macho_load(prog.dyld, DYLD_TARGET - 0x8fe00000u, &dyld)) return 1;
        g_dyld_from = dyld.from;
        g_dyld_to = dyld.bis;
        cpu.r[ESP] = stack_build(argc - i, argv + i, path, prog.mh, 1);
        cpu.eip = dyld.entrypoint;
    } else {
        cpu.r[ESP] = stack_build(argc - i, argv + i, path, prog.mh, 0);
        cpu.eip = prog.entrypoint;
    }
    g_only_interp = getenv("ROSETTA_INTERP") != 0;
    g_profile = profil;
    g_error_load = getenv("ROSETTA_STATS") ? jit_pagefault() : 0;
    if (profil) { profile_start(); signal(SIGTERM, profile_sigterm); }
    if (!g_only_interp) jit_prog(path);
    thread_start(&cpu, g_only_interp);
    gettimeofday(&g_t0, 0);
    r = g_only_interp ? run(&cpu) : jit_run(&cpu);
    if (cpu.thread_end) {                   /* main thread ended with bsdthread_terminate: the others keep running */
        jit_thread_unregister(&cpu);
        g_threads--;
        thread_release();
        pthread_exit(0);
    }
    return end_expiry(&cpu, r);
}

/* end of the guest process: translation cache, profile, statistics; exit status. A secondary thread that calls
   exit comes here via main_end (the other threads wait for the lock and end with the process) */
static int end_expiry(Cpu *c, int r)
{
    {
        struct timeval t1;
        double s;
        gettimeofday(&t1, 0);
        if (!g_only_interp) jit_end();
        if (g_profile) profile_write(g_profile);
        s = (t1.tv_sec - g_t0.tv_sec) + (t1.tv_usec - g_t0.tv_usec) * 1e-6;
        if (getenv("ROSETTA_STATS")) {
            thread_stats();
            if (g_only_interp)
                fprintf(stderr, "rosetta: %llu instructions in %.3f s = %.1f M/s\n", (unsigned long long)c->insns, s,
                        s > 0 ? c->insns / s / 1e6 : 0.0);
            else {
                task_events_info_data_t ev;
                mach_msg_type_number_t evn = TASK_EVENTS_INFO_COUNT;
                memset(&ev, 0, sizeof ev);
                task_info(mach_task_self(), TASK_EVENTS_INFO, (task_info_t)&ev, &evn);
                fprintf(stderr, "rosetta: %.3f s, %llu instructions interpreted, %d page faults (%d from disk, %d on copy; "
                        "%d until main, %d while loading the guest)\n", s,
                        (unsigned long long)c->insns, ev.faults, ev.pageins, ev.cow_faults, g_error_start,
                        g_error_load - g_error_start);
                jit_stats();
                sys_stats();
                if (getenv("ROSETTA_IMAGES")) {     /* loaded Intel images: location, requested location, name */
                    int k;
                    images_find();
                    for (k = 0; k < g_npimage; k++)
                        fprintf(stderr, "rosetta: image %08x-%08x (requested %08x%s) %s\n", g_pimage[k].start, g_pimage[k].end,
                                g_pimage[k].wish, g_pimage[k].start == g_pimage[k].wish ? "" : ", MOVED", g_pimage[k].name);
                }
            }
        }
    }
    if (g_end_signal) end_by_signal(g_end_signal);
    if (r == RUN_END) return c->end_code;
    return c->finished ? c->end_code : 127;
}

void main_profile_save(void)
{
    if (g_profile) { profile_write(g_profile); g_profile = 0; }
}

void main_end(Cpu *c, int r)
{
    fflush(stdout);
    exit(end_expiry(c, r));
}
