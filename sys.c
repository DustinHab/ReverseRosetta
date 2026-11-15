/* sys.c - BSD system calls of the guest.
 *
 * i386 Darwin: "int $0x80" or sysenter, eax = number, arguments from esp+4 (or ecx+4), 64-bit arguments in two
 * stack slots (low word first). Result in eax (64-bit results also in edx), error: CF and eax = errno
 * (BSD numbers, flag and ioctl values are the same on both sides). Pointers point into the same address space;
 * structures are converted field by field into the guest byte order. Layouts of the i386 structures:
 * tests/raw_layout.c (built by the i386 compiler, printed in the interpreter).
 */
#include "sys.h"
#include "interp.h"
#include "macho.h"
#include "mach_msg.h"
#include "jit.h"
#include "thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <poll.h>
#include <sys/sysctl.h>
#include <sys/mount.h>
#include <sys/attr.h>
#include <sys/wait.h>
#include <sys/event.h>
#include <termios.h>
#include <pthread.h>
#include <utmpx.h>
#include <signal.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach/mach_time.h>

int g_log;                        /* ROSETTA_LOG: print system calls */

/* ---------- shared read-only file mappings ----------
 * mmap(MAP_PRIVATE) of a file whose pages are already in memory costs, under Leopard, time per page of the whole
 * file (the kernel protects all of them for later copy-on-write, vm_object_copy_delayed): Foundation's __TEXT
 * ~200 us, with MAP_SHARED 5 us (tools/mmap_cost.c). Without PROT_WRITE the guest sees no difference; this is how
 * dyld maps __TEXT and __LINKEDIT. If the guest wants to write later (mprotect, vm_protect: text relocations of a
 * moved library), the range first becomes private memory with the same content. ROSETTA_NO_SHARED=1 */
#define SHARED_N 1024
static struct { uint32_t start, end; int prot; } g_shared[SHARED_N];
static int g_nshared, g_shared_on = -1;
static unsigned long g_st_shared, g_st_private;

static void shared_forget(uint32_t a, uint32_t len)
{
    int i;
    for (i = 0; i < g_nshared; i++)
        if (g_shared[i].start >= a && g_shared[i].end <= a + len) g_shared[i--] = g_shared[--g_nshared];
}

/* before write access to [a, a + len): replace shared mappings in it with private copies */
void sys_make_private(uint32_t a, uint32_t len)
{
    int i;
    for (i = 0; i < g_nshared; i++) {
        uint32_t s = g_shared[i].start, n = g_shared[i].end - s;
        void *copy;
        if (s >= a + len || g_shared[i].end <= a) continue;
        if ((copy = malloc(n)) == 0) continue;
        mprotect((void *)(uintptr_t)s, n, PROT_READ);
        memcpy(copy, (void *)(uintptr_t)s, n);
        if (mmap((void *)(uintptr_t)s, n, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == (void *)(uintptr_t)s) {
            memcpy((void *)(uintptr_t)s, copy, n);
            mprotect((void *)(uintptr_t)s, n, g_shared[i].prot);
            g_st_private++;
        }
        free(copy);
        g_shared[i--] = g_shared[--g_nshared];
    }
}

static uint32_t g_arg_base;

static inline uint32_t arg(int i) { return ld32(g_arg_base + 4 + 4 * (uint32_t)i); }
static inline uint64_t arg64(int i) { return (uint64_t)arg(i) | (uint64_t)arg(i + 1) << 32; }
#define PTR_ARG(i) ((void *)(uintptr_t)arg(i))

static void set_result(Cpu *c, uint32_t r, int errnum)
{
    uint32_t f = flags_fetch(c) & ~F_CF;
    if (errnum == EINTR && getenv("ROSETTA_HPROFILE")) fprintf(stderr, "rosetta: EINTR for the guest\n");
    if (errnum) { c->r[EAX] = (uint32_t)errnum; f |= F_CF; }
    else c->r[EAX] = r;
    flags_set(c, f);
}

#define HOST(expr) do { long r_ = (long)(expr); set_result(c, (uint32_t)r_, r_ == -1 ? errno : 0); } while (0)

#define KAUTH_ID_NONE 0xffffff9bu           /* KAUTH_UID_NONE, KAUTH_GID_NONE (sys/kauth.h): "do not change" */

/* Blocking host calls release the guest thread lock (thread.c) so other threads run in the meantime. The
   arguments are already evaluated when the wrapper is called; afterwards g_arg_base (another thread may have made
   system calls in the meantime) and errno hold their previous values again */
#define FREE_ON uint32_t from_ = g_arg_base; Cpu *fc_ = glock_release()
#define FREE_OFF do { int e_ = errno; glock_acquire(fc_); g_arg_base = from_; errno = e_; } while (0)
static ssize_t f_read(int d, void *b, size_t n) { ssize_t r; FREE_ON; r = read(d, b, n); FREE_OFF; return r; }
static ssize_t f_write(int d, const void *b, size_t n) { ssize_t r; FREE_ON; r = write(d, b, n); FREE_OFF; return r; }
static ssize_t f_readv(int d, const struct iovec *v, int n) { ssize_t r; FREE_ON; r = readv(d, v, n); FREE_OFF; return r; }
static ssize_t f_writev(int d, const struct iovec *v, int n) { ssize_t r; FREE_ON; r = writev(d, v, n); FREE_OFF; return r; }
static int f_select(int n, fd_set *a, fd_set *b, fd_set *e, struct timeval *t)
{ int r; FREE_ON; r = select(n, a, b, e, t); FREE_OFF; return r; }
static int f_poll(struct pollfd *f, unsigned n, int t) { int r; FREE_ON; r = poll(f, n, t); FREE_OFF; return r; }
static pid_t f_wait4(pid_t p, int *st, int o) { pid_t r; FREE_ON; r = wait4(p, st, o, 0); FREE_OFF; return r; }
static int f_accept(int d, struct sockaddr *a, socklen_t *l) { int r; FREE_ON; r = accept(d, a, l); FREE_OFF; return r; }
static int f_connect(int d, const struct sockaddr *a, socklen_t l) { int r; FREE_ON; r = connect(d, a, l); FREE_OFF; return r; }
static ssize_t f_recvfrom(int d, void *b, size_t n, int fl, struct sockaddr *a, socklen_t *l)
{ ssize_t r; FREE_ON; r = recvfrom(d, b, n, fl, a, l); FREE_OFF; return r; }
static ssize_t f_sendto(int d, const void *b, size_t n, int fl, const struct sockaddr *a, socklen_t l)
{ ssize_t r; FREE_ON; r = sendto(d, b, n, fl, a, l); FREE_OFF; return r; }
static ssize_t f_recvmsg(int d, struct msghdr *m, int fl) { ssize_t r; FREE_ON; r = recvmsg(d, m, fl); FREE_OFF; return r; }
static ssize_t f_sendmsg(int d, const struct msghdr *m, int fl) { ssize_t r; FREE_ON; r = sendmsg(d, m, fl); FREE_OFF; return r; }
static long f_syscall(int n, int a, int b, int d, int e, long f, int g)
{ long r; FREE_ON; r = syscall(n, a, b, d, e, f, g); FREE_OFF; return r; }
static void f_short_wait(void) { FREE_ON; usleep(1000); FREE_OFF; }
static int f_kevent(int q, const struct kevent *ch, int nch, struct kevent *ev, int nev, const struct timespec *t)
{ int r; FREE_ON; r = kevent(q, ch, nch, ev, nev, t); FREE_OFF; return r; }
static int f_fcntl_ptr(int d, int cmd, void *p) { int r; FREE_ON; r = fcntl(d, cmd, p); FREE_OFF; return r; }
static int f_ioctl(int d, unsigned long req, void *p) { int r; FREE_ON; r = ioctl(d, req, p); FREE_OFF; return r; }
static int f_sendfile(int d, int s, off_t o, off_t *l, struct sf_hdtr *h, int fl)
{ int r; FREE_ON; r = sendfile(d, s, o, l, h, fl); FREE_OFF; return r; }
static pid_t f_waitpid(pid_t p, int *st) { pid_t r; FREE_ON; r = waitpid(p, st, 0); FREE_OFF; return r; }


/* ---------- structures ---------- */

static void ts_write(uint32_t a, struct timespec t) { st32(a, (uint32_t)t.tv_sec); st32(a + 4, (uint32_t)t.tv_nsec); }

static void stat_write(uint32_t a, const struct stat *s)
{
    memset((void *)(uintptr_t)a, 0, 0x60);
    st32(a + 0x00, (uint32_t)s->st_dev);
    st32(a + 0x04, (uint32_t)s->st_ino);
    st16(a + 0x08, s->st_mode);
    st16(a + 0x0a, s->st_nlink);
    st32(a + 0x0c, s->st_uid);
    st32(a + 0x10, s->st_gid);
    st32(a + 0x14, (uint32_t)s->st_rdev);
    ts_write(a + 0x18, s->st_atimespec);
    ts_write(a + 0x20, s->st_mtimespec);
    ts_write(a + 0x28, s->st_ctimespec);
    st64(a + 0x30, (uint64_t)s->st_size);
    st64(a + 0x38, (uint64_t)s->st_blocks);
    st32(a + 0x40, (uint32_t)s->st_blksize);
    st32(a + 0x44, s->st_flags);
    st32(a + 0x48, s->st_gen);
}

static void stat64_write(uint32_t a, const struct stat64 *s)
{
    memset((void *)(uintptr_t)a, 0, 0x6c);
    st32(a + 0x00, (uint32_t)s->st_dev);
    st16(a + 0x04, s->st_mode);
    st16(a + 0x06, s->st_nlink);
    st64(a + 0x08, s->st_ino);
    st32(a + 0x10, s->st_uid);
    st32(a + 0x14, s->st_gid);
    st32(a + 0x18, (uint32_t)s->st_rdev);
    ts_write(a + 0x1c, s->st_atimespec);
    ts_write(a + 0x24, s->st_mtimespec);
    ts_write(a + 0x2c, s->st_ctimespec);
    ts_write(a + 0x34, s->st_birthtimespec);
    st64(a + 0x3c, (uint64_t)s->st_size);
    st64(a + 0x44, (uint64_t)s->st_blocks);
    st32(a + 0x4c, (uint32_t)s->st_blksize);
    st32(a + 0x50, s->st_flags);
    st32(a + 0x54, s->st_gen);
}

static void statfs_write(uint32_t a, const struct statfs *s)
{
    memset((void *)(uintptr_t)a, 0, 0x110);
    st16(a + 0x00, 0);
    st16(a + 0x02, 0);
    st32(a + 0x04, (uint32_t)s->f_bsize);
    st32(a + 0x08, (uint32_t)s->f_iosize);
    st32(a + 0x0c, (uint32_t)s->f_blocks);
    st32(a + 0x10, (uint32_t)s->f_bfree);
    st32(a + 0x14, (uint32_t)s->f_bavail);
    st32(a + 0x18, (uint32_t)s->f_files);
    st32(a + 0x1c, (uint32_t)s->f_ffree);
    st32(a + 0x20, (uint32_t)s->f_fsid.val[0]);
    st32(a + 0x24, (uint32_t)s->f_fsid.val[1]);
    st32(a + 0x28, s->f_owner);
    st16(a + 0x2e, (uint32_t)s->f_type);
    st32(a + 0x30, (uint32_t)s->f_flags);
    memcpy((void *)(uintptr_t)(a + 0x3c), s->f_fstypename, 15);
    memcpy((void *)(uintptr_t)(a + 0x4b), s->f_mntonname, 90);
    memcpy((void *)(uintptr_t)(a + 0xa5), s->f_mntfromname, 90);
}

static void statfs64_write(uint32_t a, const struct statfs64 *s)
{
    memset((void *)(uintptr_t)a, 0, 0x878);
    st32(a + 0x00, s->f_bsize);
    st32(a + 0x04, (uint32_t)s->f_iosize);
    st64(a + 0x08, s->f_blocks);
    st64(a + 0x10, s->f_bfree);
    st64(a + 0x18, s->f_bavail);
    st64(a + 0x20, s->f_files);
    st64(a + 0x28, s->f_ffree);
    st32(a + 0x30, (uint32_t)s->f_fsid.val[0]);
    st32(a + 0x34, (uint32_t)s->f_fsid.val[1]);
    st32(a + 0x38, s->f_owner);
    st32(a + 0x3c, s->f_type);
    st32(a + 0x40, s->f_flags);
    st32(a + 0x44, s->f_fssubtype);
    memcpy((void *)(uintptr_t)(a + 0x48), s->f_fstypename, 16);
    memcpy((void *)(uintptr_t)(a + 0x58), s->f_mntonname, 1024);
    memcpy((void *)(uintptr_t)(a + 0x458), s->f_mntfromname, 1024);
}

#define MEMRING 64
static struct { uint32_t a, len, eip, caller, chain[3]; char kind; } g_memring[MEMRING];
static volatile uint32_t g_njump;

void sys_mem_note(Cpu *c, char kind, uint32_t a, uint32_t len)
{
    uint32_t i = g_njump++ % MEMRING;
    g_memring[i].a = a; g_memring[i].len = len; g_memring[i].eip = c->eip; g_memring[i].caller = ld32(c->r[ESP]);
    g_memring[i].kind = kind;
    {   /* three return addresses via ebp (mach_msg -> MIG stub -> caller) */
        uint32_t bp = c->r[EBP], k;
        for (k = 0; k < 3; k++) {
            g_memring[i].chain[k] = 0;
            if (!bp || (bp & 3) || bp < 0x1000 || bp >= 0xc0000000u) continue;
            g_memring[i].chain[k] = ld32(bp + 4);
            bp = ld32(bp);
        }
    }
}

static void line_hex(char **p, uint32_t v) { int i; for (i = 28; i >= 0; i -= 4) *(*p)++ = "0123456789abcdef"[(v >> i) & 15]; }

void sys_mem_show(uint32_t addr)
{
    uint32_t n = g_njump, i, k;
    for (k = 0; k < MEMRING && k < n; k++) {
        char b[160], *p = b;
        i = (n - 1 - k) % MEMRING;
        if (k >= 8 && !(addr - g_memring[i].a < g_memring[i].len)) continue;   /* the last 8 and all hits */
        *p++ = ' '; *p++ = ' '; *p++ = g_memring[i].kind; *p++ = ' ';
        line_hex(&p, g_memring[i].a); *p++ = '+'; line_hex(&p, g_memring[i].len);
        *p++ = ' '; line_hex(&p, g_memring[i].eip); *p++ = ' '; line_hex(&p, g_memring[i].caller);
        *p++ = ' '; line_hex(&p, g_memring[i].chain[0]); *p++ = ' '; line_hex(&p, g_memring[i].chain[1]);
        *p++ = ' '; line_hex(&p, g_memring[i].chain[2]);
        if (addr - g_memring[i].a < g_memring[i].len) { *p++ = ' '; *p++ = '*'; }
        *p++ = '\n';
        write(2, b, (size_t)(p - b));
    }
}

int sys_host_range(Cpu *c, uint32_t a, uint32_t len, const char *was)
{
    uint64_t end = (uint64_t)a + (len ? len : 1);
    if (end <= 0x90000000u || a >= 0xb0000000u) return 0;
    fprintf(stderr, "rosetta: %s %08x+%x refused in host library range (eip %08x, caller %08x)\n", was, a, len,
            c->eip, ld32(c->r[ESP]));
    return 1;
}

static void ts_read(uint32_t a, struct timeval *t) { t->tv_sec = (time_t)ld32(a); t->tv_usec = (suseconds_t)ld32(a + 4); }

/* ---------- getattrlist ----------
 * The kernel packs the requested attributes in bit order (common, volume, directory, file, fork), each aligned to
 * 4 bytes, with the total length in front; references (attrreference_t) point to appended data. The layout is the
 * same for i386 and PowerPC (32 bit): the buffer is copied raw and every numeric field is swapped.
 * Kind: 'w' n words, 'q' n 64-bit values, 'r' reference (two words, data raw), 'b' n bytes raw (Finder info: on
 * disk always big endian, Intel CarbonCore swaps it itself), '-' no data. */
typedef struct { uint32_t bit; char kind; uint8_t n; } AttrType;
static const AttrType g_attr_cmn[] = {
    { 0x1, 'r', 1 }, { 0x2, 'w', 1 }, { 0x4, 'w', 2 }, { 0x8, 'w', 1 }, { 0x10, 'w', 1 }, { 0x20, 'w', 2 },
    { 0x40, 'w', 2 }, { 0x80, 'w', 2 }, { 0x100, 'w', 1 }, { 0x200, 'w', 2 }, { 0x400, 'w', 2 }, { 0x800, 'w', 2 },
    { 0x1000, 'w', 2 }, { 0x2000, 'w', 2 }, { 0x4000, 'b', 32 }, { 0x8000, 'w', 1 }, { 0x10000, 'w', 1 },
    { 0x20000, 'w', 1 }, { 0x40000, 'w', 1 }, { 0x200000, 'w', 1 }, { 0x400000, 'r', 1 }, { 0x800000, 'b', 16 },
    { 0x1000000, 'b', 16 }, { 0x2000000, 'q', 1 }, { 0x4000000, 'q', 1 }, { 0, 0, 0 } };
static const AttrType g_attr_vol[] = {
    { 0x1, 'w', 1 }, { 0x2, 'w', 1 }, { 0x4, 'q', 1 }, { 0x8, 'q', 1 }, { 0x10, 'q', 1 }, { 0x20, 'q', 1 },
    { 0x40, 'q', 1 }, { 0x80, 'w', 1 }, { 0x100, 'w', 1 }, { 0x200, 'w', 1 }, { 0x400, 'w', 1 }, { 0x800, 'w', 1 },
    { 0x1000, 'r', 1 }, { 0x2000, 'r', 1 }, { 0x4000, 'w', 1 }, { 0x8000, 'r', 1 }, { 0x10000, 'q', 1 },
    { 0x20000, 'w', 8 }, { 0x40000000, 'w', 10 }, { 0x80000000u, '-', 0 }, { 0, 0, 0 } };
static const AttrType g_attr_dir[] = { { 0x1, 'w', 1 }, { 0x2, 'w', 1 }, { 0x4, 'w', 1 }, { 0, 0, 0 } };
static const AttrType g_attr_file[] = {
    { 0x1, 'w', 1 }, { 0x2, 'q', 1 }, { 0x4, 'q', 1 }, { 0x8, 'w', 1 }, { 0x10, 'w', 1 }, { 0x20, 'w', 1 },
    { 0x40, 'w', 1 }, { 0x80, 'w', 1 }, { 0x100, 'r', 1 }, { 0x200, 'q', 1 }, { 0x400, 'q', 1 }, { 0x800, 'w', 16 },
    { 0x1000, 'q', 1 }, { 0x2000, 'q', 1 }, { 0x4000, 'w', 16 }, { 0, 0, 0 } };
static const AttrType g_attr_branch[] = { { 0x1, 'q', 1 }, { 0x2, 'q', 1 }, { 0, 0, 0 } };

/* read the guest attrlist (24 bytes) */
static void attrlist_read(uint32_t a, struct attrlist *l)
{
    l->bitmapcount = (u_short)ld16(a);
    l->reserved = (u_int16_t)ld16(a + 2);
    l->commonattr = ld32(a + 4);
    l->volattr = ld32(a + 8);
    l->dirattr = ld32(a + 12);
    l->fileattr = ld32(a + 16);
    l->forkattr = ld32(a + 20);
}

/* swap the numeric fields of the attributes in l in place, from offset o up to len. Swapping the bytes of a field
   is the same operation in both directions: getattrlist (host -> guest) and setattrlist (guest -> host). */
static void attr_swap_fields(uint32_t a, uint32_t o, uint32_t len, const struct attrlist *l, const char *call)
{
    const AttrType *tab[5] = { g_attr_cmn, g_attr_vol, g_attr_dir, g_attr_file, g_attr_branch };
    uint32_t masks[5], rest;
    int g, k;
    static int reported;
    uint8_t *p = (uint8_t *)(uintptr_t)a;
    masks[0] = l->commonattr; masks[1] = l->volattr; masks[2] = l->dirattr; masks[3] = l->fileattr; masks[4] = l->forkattr;
    for (g = 0; g < 5; g++) {
        rest = masks[g];
        for (k = 0; tab[g][k].bit && rest; k++) {
            const AttrType *t = &tab[g][k];
            uint32_t i;
            if (!(rest & t->bit)) continue;
            rest &= ~t->bit;
            switch (t->kind) {
            case 'w': for (i = 0; i < t->n && o + 4 <= len; i++, o += 4) st32(a + o, *(uint32_t *)(p + o)); break;
            case 'q': for (i = 0; i < t->n && o + 8 <= len; i++, o += 8) st64(a + o, *(uint64_t *)(p + o)); break;
            case 'r': if (o + 8 <= len) { st32(a + o, *(uint32_t *)(p + o)); st32(a + o + 4, *(uint32_t *)(p + o + 4)); } o += 8; break;
            case 'b': o += t->n; break;
            default: break;
            }
        }
        if (rest) {                         /* unknown attribute: size unknown, the rest stays as it is */
            if (!reported++) fprintf(stderr, "rosetta: %s: unknown attributes %u:%08x\n", call, g, rest);
            break;
        }
    }
}

/* swap the result of one entry (length in front, at guest address a, at most size bytes, already copied raw);
   returns the length of the entry */
static uint32_t attr_swap(uint32_t a, uint32_t size, const struct attrlist *l)
{
    uint32_t len;
    if (size < 4) return 0;
    len = *(uint32_t *)(uintptr_t)a;     /* host byte order */
    st32(a, len);
    if (len > size) len = size;
    attr_swap_fields(a, 4, len, l, "getattrlist");
    return len;
}

/* ---------- sysctl ---------- */

/* guest OID "sysctl.proc_native" (tag for the Rosetta mode of CoreGraphics etc.) */
#define OID_PROC_NATIVE 0x7e57001u

/* type of an OID (for swapping); remembered per OID: the query was a second system call per guest sysctl */
static int sysctl_type(const int *name, unsigned n)
{
    static struct { unsigned n; int name[8], type; } memo[32];
    static int nmemo;
    int q[CTL_MAXNAME + 2], k, type;
    char buf[256];
    size_t l = sizeof buf;
    if (n > CTL_MAXNAME) return 0;
    for (k = 0; k < nmemo; k++)
        if (memo[k].n == n && !memcmp(memo[k].name, name, n * sizeof(int))) return memo[k].type;
    q[0] = 0; q[1] = 4;                     /* oidfmt */
    memcpy(q + 2, name, n * sizeof(int));
    if (sysctl(q, n + 2, buf, &l, 0, 0) < 0 || l < 4) return 0;
    type = *(int *)buf & 0xf;                /* CTLTYPE */
    if (n <= 8) {
        k = nmemo < 32 ? nmemo++ : (int)(name[n - 1] & 31);
        memo[k].n = n;
        memcpy(memo[k].name, name, n * sizeof(int));
        memo[k].type = type;
    }
    return type;
}

/* in-place byte swap of guest memory (host order -> guest order) */
static inline void sw16(uint32_t a) { st16(a, *(uint16_t *)(uintptr_t)a); }
static inline void sw32(uint32_t a) { st32(a, *(uint32_t *)(uintptr_t)a); }
static inline void sw64(uint32_t a) { st64(a, *(uint64_t *)(uintptr_t)a); }
static void sw32_range(uint32_t a, uint32_t from, uint32_t to) { uint32_t o; for (o = from; o + 4 <= to; o += 4) sw32(a + o); }

/* struct kinfo_proc (492 bytes, the same layout for i386 and PowerPC): extern_proc (196) and eproc (296); words,
   except the bytes p_stat, p_priority..p_comm, pc_lock, e_wmesg, e_login and the shorts */
#define KINFO_PROC_SIZE 492
typedef char kinfo_proc_size_check[sizeof(struct kinfo_proc) == KINFO_PROC_SIZE ? 1 : -1];
typedef char kinfo_proc_layout_check[offsetof(struct kinfo_proc, kp_eproc.e_login) == 196 + 268 ? 1 : -1];
static void kinfo_proc_swap(uint32_t a)
{
    uint32_t e = a + 196, o;
    sw32_range(a, 0, 20);                    /* p_un, p_vmspace, p_sigacts, p_flag */
    sw32_range(a, 24, 104);                  /* p_pid .. p_rtime */
    for (o = 104; o < 128; o += 8) sw64(a + o);   /* p_uticks, p_sticks, p_iticks */
    sw32_range(a, 128, 160);                 /* p_traceflag .. p_sigcatch */
    sw32(a + 180); sw32(a + 184); sw16(a + 188); sw16(a + 190); sw32(a + 192);
    sw32(e); sw32(e + 4);                    /* e_paddr, e_sess; pc_lock (72 bytes) stays */
    sw32_range(e, 80, 104);                  /* pc_ucred, p_ruid, p_svuid, p_rgid, p_svgid, p_refcnt */
    sw32(e + 104); sw32(e + 108); sw16(e + 112);   /* cr_ref, cr_uid, cr_ngroups */
    sw32_range(e, 116, 228);                 /* cr_groups, e_vm, e_ppid, e_pgid */
    sw16(e + 228); sw32(e + 232); sw32(e + 236); sw32(e + 240);   /* e_jobc, e_tdev, e_tpgid, e_tsess */
    sw32(e + 252); sw16(e + 256); sw16(e + 258); sw16(e + 260); sw32(e + 264);
    sw32_range(e, 280, 296);                 /* e_spare */
}

/* routing messages (sysctl NET_RT_*: getifaddrs, if_nameindex, route tables): per message u16 length, version,
   type, then a header that depends on the type, then the socket addresses selected by its address mask. Socket
   addresses are in network order except sdl_index (AF_LINK) and sin6_scope_id (AF_INET6). */
static void rt_addrs_swap(uint32_t p, uint32_t end, uint32_t addrs)
{
    int k;
    for (k = 0; k < 8 && p < end; k++) {     /* RTAX_MAX */
        uint32_t len;
        if (!(addrs & (1u << k))) continue;
        len = ld8(p);
        if (ld8(p + 1) == AF_LINK && len >= 4 && p + 4 <= end) sw16(p + 2);
        if (ld8(p + 1) == AF_INET6 && len >= 28 && p + 28 <= end) sw32(p + 24);
        p += len ? (len + 3) & ~3u : 4;
    }
}

static void rt_msgs_swap(uint32_t a, uint32_t n)
{
    uint32_t p = a, end = a + n;
    while (p + 4 <= end) {
        uint32_t len = *(uint16_t *)(uintptr_t)p, type = ld8(p + 3), head = 0, addrs = 0, o;
        if (len < 4 || p + len > end) break;
        switch (type) {
        case 0xe:                            /* RTM_IFINFO: if_msghdr + if_data (8 bytes, then words) */
            addrs = *(uint32_t *)(uintptr_t)(p + 4); head = 112;
            sw32(p + 4); sw32(p + 8); sw16(p + 12); sw32_range(p, 24, 112);
            break;
        case 0x12:                           /* RTM_IFINFO2: if_msghdr2 + if_data64 */
            addrs = *(uint32_t *)(uintptr_t)(p + 4); head = 160;
            sw32(p + 4); sw32(p + 8); sw16(p + 12); sw32_range(p, 16, 32);
            sw32(p + 40); sw32(p + 44);
            for (o = 48; o < 144; o += 8) sw64(p + o);
            sw32_range(p, 144, 160);
            break;
        case 0xc: case 0xd:                  /* RTM_NEWADDR, RTM_DELADDR: ifa_msghdr */
            addrs = *(uint32_t *)(uintptr_t)(p + 4); head = 20;
            sw32(p + 4); sw32(p + 8); sw16(p + 12); sw32(p + 16);
            break;
        case 0xf: case 0x10: case 0x13:      /* RTM_NEWMADDR, RTM_DELMADDR: ifma_msghdr; RTM_NEWMADDR2 + refcount */
            addrs = *(uint32_t *)(uintptr_t)(p + 4); head = type == 0x13 ? 20 : 16;
            sw32(p + 4); sw32(p + 8); sw16(p + 12); if (type == 0x13) sw32(p + 16);
            break;
        default:
            if ((type >= 1 && type <= 0xb) || type == 0x14) {   /* rt_msghdr, rt_msghdr2: index, then words */
                addrs = *(uint32_t *)(uintptr_t)(p + 12); head = 92;
                sw16(p + 4); sw32_range(p, 8, 92);
            }
        }
        sw16(p);
        if (head && head <= len) rt_addrs_swap(p + head, p + len, addrs);
        p += len;
    }
}

/* the guest's own arguments (main.c, sys_set_args): the kernel knows this process as rosetta */
static int g_args_n;
static char **g_args_v;
static const char *g_args_path;

void sys_set_args(int argc, char **argv, const char *path) { g_args_n = argc; g_args_v = argv; g_args_path = path; }

/* KERN_PROCARGS2 of the own process as the kernel would write it for the guest: argc, executable path, NULs up to a
   word boundary, the arguments, the environment; returns the length (the content only if it fits into max) */
static uint32_t procargs2_own(uint32_t a, uint32_t max)
{
    extern char **environ;
    uint32_t need = 4, o, k;
    char **e;
    need += (uint32_t)strlen(g_args_path) + 1;
    need = (need + 3) & ~3u;
    for (k = 0; k < (uint32_t)g_args_n; k++) need += (uint32_t)strlen(g_args_v[k]) + 1;
    for (e = environ; *e; e++) need += (uint32_t)strlen(*e) + 1;
    if (!a || need > max) return need;
    st32(a, (uint32_t)g_args_n);
    o = 4;
    strcpy((char *)(uintptr_t)(a + o), g_args_path);
    o += (uint32_t)strlen(g_args_path) + 1;
    while (o & 3) *(char *)(uintptr_t)(a + o++) = 0;
    for (k = 0; k < (uint32_t)g_args_n; k++) { strcpy((char *)(uintptr_t)(a + o), g_args_v[k]); o += (uint32_t)strlen(g_args_v[k]) + 1; }
    for (e = environ; *e; e++) { strcpy((char *)(uintptr_t)(a + o), *e); o += (uint32_t)strlen(*e) + 1; }
    return need;
}

/* results of sysctl that are structures (CTLTYPE_STRUCT or node handlers): converted by name; 1 = done */
static int sysctl_struct_swap(const int *name, uint32_t n, uint32_t a, uint32_t l)
{
    uint32_t k;
    if (n >= 3 && name[0] == CTL_KERN && name[1] == KERN_PROC) {
        for (k = 0; k + KINFO_PROC_SIZE <= l; k += KINFO_PROC_SIZE) {
            kinfo_proc_swap(a + k);
            if ((pid_t)ld32(a + k + 24) == getpid() && g_args_path) {   /* p_comm of the own process: the guest's name */
                const char *s = strrchr(g_args_path, '/') ? strrchr(g_args_path, '/') + 1 : g_args_path;
                strncpy((char *)(uintptr_t)(a + k + 163), s, 16);
                *(char *)(uintptr_t)(a + k + 179) = 0;
            }
        }
        return 1;
    }
    if (n == 3 && name[0] == CTL_KERN && name[1] == KERN_PROCARGS2) { if (l >= 4) sw32(a); return 1; }   /* argc */
    if (n == 2 && name[0] == CTL_KERN && name[1] == KERN_CLOCKRATE) { sw32_range(a, 0, l); return 1; }  /* clockinfo */
    if (n == 2 && name[0] == CTL_VM && name[1] == VM_LOADAVG) { sw32_range(a, 0, l); return 1; }         /* loadavg */
    if (n == 2 && name[0] == CTL_VM && name[1] == VM_SWAPUSAGE && l >= 32) {                             /* xsw_usage */
        sw64(a); sw64(a + 8); sw64(a + 16); sw32(a + 24); sw32(a + 28);
        return 1;
    }
    if (n >= 5 && name[0] == CTL_NET && name[1] == PF_ROUTE && name[2] == 0) {
        if (name[4] == NET_RT_STAT) { for (k = 0; k + 2 <= l; k += 2) sw16(a + k); }   /* rtstat: shorts */
        else rt_msgs_swap(a, l);
        return 1;
    }
    return 0;
}

/* The processor the guest sees: the Intel Core Duo that CPUID and the commpage describe (Apple's Rosetta likewise
   showed PowerPC programs a PowerPC). Values by name (sysctlbyname gets fake OIDs for them); names that only a
   PowerPC has are hidden. ROSETTA_HOST_CPU=1 shows the real processor. host_info: mach_msg.c. */
#define OID_IDENTITY 0x7e57100u
static const struct { const char *name; char kind; uint32_t value; const char *text; } g_identity[] = {
    { "hw.cputype", 'i', 7, 0 }, { "hw.cpusubtype", 'i', 4, 0 }, { "hw.cpufamily", 'i', 0x73d67300u, 0 },
    { "hw.cpu64bit_capable", 'i', 0, 0 }, { "hw.optional.floatingpoint", 'i', 1, 0 }, { "hw.optional.mmx", 'i', 1, 0 },
    { "hw.optional.sse", 'i', 1, 0 }, { "hw.optional.sse2", 'i', 1, 0 }, { "hw.optional.sse3", 'i', 1, 0 },
    { "hw.optional.supplementalsse3", 'i', 0, 0 }, { "hw.optional.sse4_1", 'i', 0, 0 }, { "hw.optional.sse4_2", 'i', 0, 0 },
    { "hw.optional.x86_64", 'i', 0, 0 },
    { "hw.optional.altivec", 'x', 0, 0 }, { "hw.optional.64bitops", 'x', 0, 0 }, { "hw.optional.graphicsops", 'x', 0, 0 },
    { "hw.optional.stfiwx", 'x', 0, 0 }, { "hw.optional.dcba", 'x', 0, 0 }, { "hw.optional.datastreams", 'x', 0, 0 },
    { "machdep.cpu.vendor", 's', 0, "GenuineIntel" },
    { "machdep.cpu.brand_string", 's', 0, "Intel(R) Core(TM) Duo CPU (ReverseRosetta on PowerPC)" },
    { "machdep.cpu.family", 'i', 6, 0 }, { "machdep.cpu.model", 'i', 14, 0 }, { "machdep.cpu.stepping", 'i', 8, 0 },
    { "machdep.cpu.features", 's', 0, "FPU VME DE PSE TSC MSR PAE MCE CX8 APIC SEP MTRR PGE MCA CMOV PAT CLFSH MMX FXSR SSE SSE2 SSE3" },
};
#define IDENTITY_N (sizeof g_identity / sizeof g_identity[0])
int g_host_cpu = -1;                        /* ROSETTA_HOST_CPU: the guest sees the PowerPC */

static int identity_find(const char *s, uint32_t len)
{
    uint32_t k;
    if (g_host_cpu < 0) g_host_cpu = getenv("ROSETTA_HOST_CPU") != 0;
    if (g_host_cpu) return -1;
    while (len && !s[len - 1]) len--;        /* with or without the NUL */
    for (k = 0; k < IDENTITY_N; k++)
        if (strlen(g_identity[k].name) == len && !strncmp(g_identity[k].name, s, len)) return (int)k;
    return -1;
}

static void sys_sysctl(Cpu *c)
{
    uint32_t gname = arg(0), n = arg(1), prev = arg(2), oldlen = arg(3), fresh = arg(4), newlen = arg(5), i;
    int name[CTL_MAXNAME];
    size_t l = 0;
    int r, type;
    if (n == 0 || n > CTL_MAXNAME) { set_result(c, 0, EINVAL); return; }
    for (i = 0; i < n; i++) name[i] = (int)ld32(gname + 4 * i);
    if (oldlen) l = ld32(oldlen);
    if (g_log) {
        fprintf(stderr, "[sysctl");
        for (i = 0; i < n; i++) fprintf(stderr, " %d", name[i]);
        if (fresh && n == 2 && name[0] == 0 && name[1] == 3) fprintf(stderr, " \"%.*s\"", (int)newlen, (const char *)(uintptr_t)fresh);
        fprintf(stderr, "] old %s, %u bytes\n", prev ? "yes" : "no", (unsigned)l);
    }

    /* names to OIDs (0.3): intercept the Rosetta tag, otherwise convert the result OIDs */
    if (n == 2 && name[0] == 0 && name[1] == 3) {
        if (fresh && newlen >= 18 && !strncmp((const char *)(uintptr_t)fresh, "sysctl.proc_native", newlen)) {
            if (l < 8) { set_result(c, 0, ENOMEM); return; }
            st32(prev, 0); st32(prev + 4, OID_PROC_NATIVE);
            st32(oldlen, 8);
            set_result(c, 0, 0);
            return;
        }
        if (fresh && newlen) {
            int k = identity_find((const char *)(uintptr_t)fresh, newlen);
            if (k >= 0) {
                if (g_identity[k].kind == 'x') { set_result(c, 0, ENOENT); return; }
                if (l < 8 || !prev) { set_result(c, 0, ENOMEM); return; }
                st32(prev, 0); st32(prev + 4, OID_IDENTITY + (uint32_t)k);
                st32(oldlen, 8);
                set_result(c, 0, 0);
                return;
            }
        }
        r = sysctl(name, n, (void *)(uintptr_t)prev, oldlen ? &l : 0, (void *)(uintptr_t)fresh, newlen);
        if (r < 0) { set_result(c, 0, errno); return; }
        for (i = 0; prev && i + 4 <= l; i += 4) st32(prev + i, *(uint32_t *)(uintptr_t)(prev + i));
        if (oldlen) st32(oldlen, (uint32_t)l);
        set_result(c, 0, 0);
        return;
    }
    if (n == 2 && name[0] == 0 && (uint32_t)name[1] - OID_IDENTITY < IDENTITY_N) {
        uint32_t k = (uint32_t)name[1] - OID_IDENTITY;
        if (g_identity[k].kind == 's') {
            uint32_t len = (uint32_t)strlen(g_identity[k].text) + 1;
            if (prev) { if (l < len) { set_result(c, 0, ENOMEM); return; } memcpy((void *)(uintptr_t)prev, g_identity[k].text, len); }
            if (oldlen) st32(oldlen, len);
        } else {
            if (prev) { if (l < 4) { set_result(c, 0, ENOMEM); return; } st32(prev, g_identity[k].value); }
            if (oldlen) st32(oldlen, 4);
        }
        set_result(c, 0, fresh ? EPERM : 0);
        return;
    }
    if (n == 2 && name[0] == 0 && (uint32_t)name[1] == OID_PROC_NATIVE) {
        if (prev && l >= 4) st32(prev, 0);    /* not native: the frameworks switch to Rosetta mode */
        if (oldlen) st32(oldlen, 4);
        set_result(c, 0, 0);
        return;
    }
    /* values an Intel program expects */
    if (n == 2 && name[0] == CTL_KERN && name[1] == 45) {
        /* KERN_EXEC/KERN_CLASSICHANDLER: on an Intel Mac the path of the PowerPC translator. The i386 libobjc reads
           Objective-C categories only if something is here (the PowerPC kernel returns an empty string) */
        static const char t[] = "/usr/libexec/oah/translate";
        if (prev) { if (l < sizeof t) { set_result(c, 0, ENOMEM); return; } memcpy((void *)(uintptr_t)prev, t, sizeof t); }
        if (oldlen) st32(oldlen, sizeof t);
        set_result(c, 0, 0);
        return;
    }
    if (n == 3 && name[0] == CTL_KERN && name[1] == KERN_PROCARGS2 && name[2] == getpid() && g_args_path) {
        uint32_t need = procargs2_own(prev, (uint32_t)l);
        if (prev && need > l) { set_result(c, 0, ENOMEM); return; }
        if (oldlen) st32(oldlen, need);
        set_result(c, 0, 0);
        return;
    }
    if (n == 2 && name[0] == CTL_HW && name[1] == HW_BYTEORDER) {   /* the guest's own byte order */
        if (prev && l >= 4) st32(prev, 1234);
        if (oldlen) st32(oldlen, 4);
        set_result(c, 0, 0);
        return;
    }
    if (n == 2 && name[0] == CTL_HW && name[1] == HW_MACHINE) {
        static const char m[] = "i386";
        if (prev && l >= sizeof m) memcpy((void *)(uintptr_t)prev, m, sizeof m);
        if (oldlen) st32(oldlen, sizeof m);
        set_result(c, 0, 0);
        return;
    }
    /* the length pointer goes along also without a buffer: that is how the guest asks for the size it needs */
    r = sysctl(name, n, (void *)(uintptr_t)prev, oldlen ? &l : 0, (void *)(uintptr_t)fresh, newlen);
    if (r < 0) {
        int e = errno;
        if (e == ENOMEM && oldlen) st32(oldlen, (uint32_t)l);
        set_result(c, 0, e);
        return;
    }
    if (prev && l && sysctl_struct_swap(name, n, prev, (uint32_t)l)) type = -1;
    else type = prev && l ? sysctl_type(name, n) : 0;
    if (prev && type >= 0) {
        if (type == CTLTYPE_INT)
            for (i = 0; i + 4 <= l; i += 4) st32(prev + i, *(uint32_t *)(uintptr_t)(prev + i));
        else if (type == CTLTYPE_QUAD)
            for (i = 0; i + 8 <= l; i += 8) st64(prev + i, *(uint64_t *)(uintptr_t)(prev + i));
        else if (type != CTLTYPE_STRING && l == 4)
            st32(prev, *(uint32_t *)(uintptr_t)prev);
        else if (type != CTLTYPE_STRING && l == 8 && name[0] == CTL_KERN && name[1] == KERN_BOOTTIME) {
            st32(prev, *(uint32_t *)(uintptr_t)prev); st32(prev + 4, *(uint32_t *)(uintptr_t)(prev + 4));
        }
    }
    if (oldlen) st32(oldlen, (uint32_t)l);
    set_result(c, 0, 0);
}

/* ---------- directories ---------- */

/* getdirentries (old dirent): d_ino (4), d_reclen (2), d_type, d_namlen, name */
static void dirent_convert(uint32_t a, long n)
{
    uint32_t p = a, end = a + (uint32_t)n;
    while (p + 8 <= end) {
        uint32_t ino = *(uint32_t *)(uintptr_t)p;
        uint16_t len = *(uint16_t *)(uintptr_t)(p + 4);
        st32(p, ino);
        st16(p + 4, len);
        if (len == 0) break;
        p += len;
    }
}

/* getdirentries64: d_ino (8), d_seekoff (8), d_reclen (2), d_namlen (2), d_type, name */
static void dirent64_convert(uint32_t a, long n)
{
    uint32_t p = a, end = a + (uint32_t)n;
    while (p + 21 <= end) {
        uint64_t ino = *(uint64_t *)(uintptr_t)p, off = *(uint64_t *)(uintptr_t)(p + 8);
        uint16_t len = *(uint16_t *)(uintptr_t)(p + 16), nl = *(uint16_t *)(uintptr_t)(p + 18);
        st64(p, ino); st64(p + 8, off); st16(p + 16, len); st16(p + 18, nl);
        if (len == 0) break;
        p += len;
    }
}

/* ---------- signals to the guest ----------
 * sigaction records handler, trampoline (_sigtramp of libc), mask and flags. For signals with a handler a host
 * handler catches the signal and only marks it pending (g_sig_pending); SIG_IGN/SIG_DFL also apply in the host
 * (SIGPIPE ignored -> EPIPE; SIG_DFL only while the guest does not block the signal, see sig_sync). Own signals (kill, __pthread_kill) are marked pending the same way. Delivery happens
 * at the dispatcher (jit.c, jit_run) with a stack frame like the kernel's (xnu sendsig, 32 bit): mcontext32
 * (600 bytes), siginfo (64), ucontext32 (32), sigframe32 (retaddr -1, catcher, sigstyle, sig, sinfo, uctx; esp
 * points to it, aligned to 16 bytes after the return word), eip = trampoline. libc calls the handler and then
 * sigreturn(uctx, sigstyle).
 * Limit: a loop in translated code without a system call never passes the dispatcher. */

static uint32_t g_handler[32], g_handler_mask[32], g_handler_flags[32], g_tramp;
volatile uint32_t g_sig_pending;
uint32_t g_sig_mask;
static uint32_t g_sig_after, g_sig_after_valid;   /* sigsuspend: mask that applies again after the handler */
static int g_sig_eip_set;                   /* sigreturn has set eip/esp: sysenter leaves them as they are */
static struct { int code, pid, uid, status; uint32_t addr, val; } g_sig_info[32];

#define MCTX32 600
#define UC_TRAD 1
#define UC_FLAVOR 30
#define SIG_BIT(s) (1u << ((s) - 1))           /* like sigmask(): signal n = bit n-1 (also g_sig_pending) */
#define SIG_NEVER (SIG_BIT(SIGKILL) | SIG_BIT(SIGSTOP))

/* wakes threads waiting in sigsuspend/sigwait while other guest threads exist (created by the first such wait;
   writing to it is async-signal-safe, the read end is non-blocking and drained by the waiter) */
static int g_sig_pipe[2] = { -1, -1 };

static void sig_wake(void)
{
    if (g_sig_pipe[1] >= 0) { int e = errno; (void)write(g_sig_pipe[1], "", 1); errno = e; }
}

/* descriptors of the translator itself: closing them is a no-op for the guest, and before the guest takes their
   number with dup2 they move to another one */
static int sys_own_fd(int fd) { return fd >= 0 && (fd == g_sig_pipe[0] || fd == g_sig_pipe[1]); }

static void sys_own_fd_move(int fd)
{
    int k;
    for (k = 0; k < 2; k++)
        if (fd >= 0 && g_sig_pipe[k] == fd) {
            int moved = fcntl(fd, F_DUPFD, getdtablesize() > 64 ? getdtablesize() - 32 : 3);
            if (moved < 0) continue;
            fcntl(moved, F_SETFD, FD_CLOEXEC);
            fcntl(moved, F_SETFL, O_NONBLOCK);
            g_sig_pipe[k] = moved;           /* the old number is closed by the guest's dup2 */
        }
}

static void sig_host(int sig, siginfo_t *si, void *k)
{
    (void)k;
    if (sig <= 0 || sig >= 32) return;
    if (si) {
        g_sig_info[sig].code = si->si_code; g_sig_info[sig].pid = si->si_pid; g_sig_info[sig].uid = (int)si->si_uid;
        g_sig_info[sig].status = si->si_status; g_sig_info[sig].addr = (uint32_t)(uintptr_t)si->si_addr;
        g_sig_info[sig].val = (uint32_t)si->si_value.sival_int;
    }
    __sync_fetch_and_or(&g_sig_pending, SIG_BIT(sig));
    sig_wake();
}

/* signals the translator needs itself (fault catcher, sampling profile) or that nobody catches */
static int sig_own(int sig)
{
    return sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGKILL || sig == SIGSTOP ||
           ((sig == SIGALRM || sig == SIGPROF) && getenv("ROSETTA_HPROFILE"));
}

/* SIG_DFL signals that the guest blocks are caught by the host as well: they are only marked pending, and the
   default action happens when the guest unblocks them (sig_deliver -> sig_default), or sigwait takes them. The
   guest mask exists only in the translator; without this a SIGUSR1 blocked by the guest would still terminate the
   process. Unblocked SIG_DFL signals keep the host default action (no cost). Called after every change of a
   handler or of the mask. */
static uint32_t g_dfl_caught;               /* SIG_DFL signals the host currently catches */

static void sig_sync(void)
{
    int sig;
    for (sig = 1; sig < 32; sig++) {
        uint32_t b = SIG_BIT(sig);
        int want;
        struct sigaction sa;
        if (sig_own(sig) || g_handler[sig] != 0) continue;
        want = (g_sig_mask & b) != 0;
        if (want == ((g_dfl_caught & b) != 0)) continue;
        memset(&sa, 0, sizeof sa);
        if (want) { sa.sa_sigaction = sig_host; sa.sa_flags = SA_SIGINFO | SA_RESTART; g_dfl_caught |= b; }
        else { sa.sa_handler = SIG_DFL; g_dfl_caught &= ~b; }
        sigaction(sig, &sa, 0);
    }
}

/* host behavior after the guest handler */
static void sig_host_set(int sig)
{
    struct sigaction sa;
    if (sig_own(sig)) return;
    memset(&sa, 0, sizeof sa);
    if (g_handler[sig] == 0) sa.sa_handler = SIG_DFL;
    else if (g_handler[sig] == 1) sa.sa_handler = SIG_IGN;
    else {
        sa.sa_sigaction = sig_host;
        sa.sa_flags = SA_SIGINFO | ((g_handler_flags[sig] & SA_RESTART) ? SA_RESTART : 0);
    }
    sigaction(sig, &sa, 0);
    g_dfl_caught &= ~SIG_BIT(sig);
    sig_sync();
}

static void sig_schedule(int sig, int code)
{
    if (sig <= 0 || sig >= 32) return;
    g_sig_info[sig].code = code; g_sig_info[sig].pid = getpid(); g_sig_info[sig].uid = (int)getuid();
    g_sig_info[sig].status = 0; g_sig_info[sig].addr = 0; g_sig_info[sig].val = 0;
    __sync_fetch_and_or(&g_sig_pending, SIG_BIT(sig));
    sig_wake();
}

/* SIG_DFL: ignore, stop or terminate the host with the same signal (exit status to the shell as with native) */
static void sig_default(Cpu *c, int sig)
{
    sigset_t m;
    switch (sig) {
    case SIGCHLD: case SIGURG: case SIGCONT: case SIGWINCH: case SIGINFO: case SIGIO:
        return;
    case SIGTSTP: case SIGTTIN: case SIGTTOU:
        kill(getpid(), SIGSTOP);
        return;
    }
    fflush(stdout);
    main_profile_save();
    if (!sig_own(sig)) {
        signal(sig, SIG_DFL);
        sigemptyset(&m);
        sigaddset(&m, sig);
        sigprocmask(SIG_UNBLOCK, &m, 0);
        kill(getpid(), sig);
    }
    c->finished = 1;                         /* still alive (own signal): report as terminated */
    c->end_code = 128 + sig;
}

/* synchronous exception of the guest found by the interpreter (division error, ud2, int3, hlt) at c->eip: with a
   handler that is not blocked, the signal is marked pending and the dispatcher delivers it before the next
   instruction (returns 1). Otherwise the program ends; main.c then terminates rosetta with the same signal, as the
   kernel would terminate the native process (returns 0). */
int g_end_signal;

int sys_signal(Cpu *c, int sig, int code, const char *reason)
{
    if (g_handler[sig] > 1 && !(g_sig_mask & SIG_BIT(sig))) {
        sig_schedule(sig, code);
        g_sig_info[sig].addr = c->eip;
        return 1;
    }
    state_show(c, reason);
    fprintf(stderr, "rosetta: signal %d to the program, terminated\n", sig);
    c->finished = 1;
    c->end_code = 128 + sig;
    g_end_signal = sig;
    return 0;
}

/* deliver a pending, unblocked signal (called at the dispatcher, guest state complete in Cpu) */
void sig_deliver(Cpu *c)
{
    uint32_t ready = g_sig_pending & ~g_sig_mask, h, sp, mctx, sinfo, uctx, fp, f, saved, fl;
    int sig, k;
    if (!ready) return;
    for (sig = 1; sig < 32 && !(ready & SIG_BIT(sig)); sig++) ;
    if (sig >= 32) return;
    __sync_fetch_and_and(&g_sig_pending, ~SIG_BIT(sig));
    h = g_handler[sig];
    if (h <= 1) {                           /* SIG_IGN or SIG_DFL: no handler that would restore the mask of sigsuspend */
        if (g_sig_after_valid) { g_sig_mask = g_sig_after; g_sig_after_valid = 0; sig_sync(); }
        if (h == 0) sig_default(c, sig);
        return;
    }
    saved = g_sig_after_valid ? g_sig_after : g_sig_mask;
    g_sig_after_valid = 0;
    xmm_intel(c);
    fl = flags_fetch(c);
    sp = c->r[ESP];
    sp -= MCTX32; mctx = sp;
    sp -= 64; sinfo = sp;
    sp -= 32; uctx = sp;
    sp -= 24;
    fp = (sp & ~15u) - 4;
    /* sigframe32 */
    st32(fp, 0xffffffffu); st32(fp + 4, h);
    st32(fp + 8, (g_handler_flags[sig] & SA_SIGINFO) ? UC_FLAVOR : UC_TRAD);
    st32(fp + 12, (uint32_t)sig); st32(fp + 16, sinfo); st32(fp + 20, uctx);
    /* ucontext32 */
    st32(uctx, 0); st32(uctx + 4, saved); st32(uctx + 8, fp); st32(uctx + 12, 0); st32(uctx + 16, 0);
    st32(uctx + 20, 0); st32(uctx + 24, MCTX32); st32(uctx + 28, mctx);
    /* mcontext32: exception (12), registers (64): eax ebx ecx edx edi esi ebp esp ss eflags eip cs ds es fs gs */
    memset((void *)(uintptr_t)mctx, 0, MCTX32);
    st32(mctx + 12, c->r[EAX]); st32(mctx + 16, c->r[EBX]); st32(mctx + 20, c->r[ECX]); st32(mctx + 24, c->r[EDX]);
    st32(mctx + 28, c->r[EDI]); st32(mctx + 32, c->r[ESI]); st32(mctx + 36, c->r[EBP]); st32(mctx + 40, c->r[ESP]);
    st32(mctx + 44, 0x1f); st32(mctx + 48, fl); st32(mctx + 52, c->eip); st32(mctx + 56, 0x17);
    st32(mctx + 60, 0x1f); st32(mctx + 64, 0x1f); st32(mctx + 68, 0x1f); st32(mctx + 72, 0x37);
    /* floating point (524 from +76): fcw +8, fsw +10, mxcsr +32, stmm0 +40 (here: double in guest byte order),
       xmm0 +168; TOP and depth of the x87 stack in the reserved area (+296) for sigreturn */
    f = mctx + 76;
    st16(f + 8, c->fpu_ctl); st16(f + 10, c->fpu_status); st32(f + 32, c->mxcsr); st32(f + 36, 0xffff);
    for (k = 0; k < 8; k++) {
        uint64_t w;
        memcpy(&w, &c->st[k], 8);
        st64(f + 40 + 16 * (uint32_t)k, w);
        memcpy((void *)(uintptr_t)(f + 168 + 16 * (uint32_t)k), c->xmm[k].b, 16);
    }
    st32(f + 296, 0x524f5354u); st32(f + 300, (uint32_t)c->st_top); st32(f + 304, c->x87_depth);
    /* siginfo: si_signo, si_errno, si_code, si_pid, si_uid, si_status, si_addr, si_value */
    memset((void *)(uintptr_t)sinfo, 0, 64);
    st32(sinfo, (uint32_t)sig); st32(sinfo + 8, (uint32_t)g_sig_info[sig].code);
    st32(sinfo + 12, (uint32_t)g_sig_info[sig].pid); st32(sinfo + 16, (uint32_t)g_sig_info[sig].uid);
    st32(sinfo + 20, (uint32_t)g_sig_info[sig].status); st32(sinfo + 24, g_sig_info[sig].addr);
    st32(sinfo + 28, g_sig_info[sig].val);
    /* mask in the handler, SA_RESETHAND */
    g_sig_mask = (g_sig_mask | g_handler_mask[sig] | ((g_handler_flags[sig] & SA_NODEFER) ? 0 : SIG_BIT(sig))) & ~SIG_NEVER;
    if (g_handler_flags[sig] & SA_RESETHAND) { g_handler[sig] = 0; sig_host_set(sig); }
    sig_sync();
    c->r[ESP] = fp;
    c->eip = g_tramp;
    if (g_log) fprintf(stderr, "[signal %d] handler %08x via %08x, frame %08x\n", sig, h, g_tramp, fp);
}

/* sigreturn(uctx, sigstyle): state from the stack frame (the handler may have changed it) */
static void sig_back(Cpu *c, uint32_t uctx)
{
    uint32_t mctx = ld32(uctx + 28), f = mctx + 76;
    int k;
    g_sig_mask = ld32(uctx + 4) & ~SIG_NEVER;
    sig_sync();
    c->r[EAX] = ld32(mctx + 12); c->r[EBX] = ld32(mctx + 16); c->r[ECX] = ld32(mctx + 20); c->r[EDX] = ld32(mctx + 24);
    c->r[EDI] = ld32(mctx + 28); c->r[ESI] = ld32(mctx + 32); c->r[EBP] = ld32(mctx + 36); c->r[ESP] = ld32(mctx + 40);
    flags_set(c, ld32(mctx + 48) & ~F_TF);
    c->eip = ld32(mctx + 52);
    c->fpu_ctl = ld16(f + 8); c->fpu_status = ld16(f + 10); c->mxcsr = ld32(f + 32);
    for (k = 0; k < 8; k++) memcpy(c->xmm[k].b, (const void *)(uintptr_t)(f + 168 + 16 * (uint32_t)k), 16);
    if (ld32(f + 296) == 0x524f5354u) {     /* written by us: restore the x87 stack */
        for (k = 0; k < 8; k++) { uint64_t w = ld64(f + 40 + 16 * (uint32_t)k); memcpy(&c->st[k], &w, 8); }
        c->st_top = (int)(ld32(f + 300) & 7); c->x87_depth = ld32(f + 304) > 8 ? 8 : ld32(f + 304);
    }
    g_sig_eip_set = 1;
}

/* wait until one of the signals in bits is pending for the guest (sigsuspend, sigwait) */
static void sig_wait_for(uint32_t bits)
{
    sigset_t all, before;
    if (g_threads > 1) {
        /* the host delivers the signal to any thread: wait on the wake-up pipe (lock free) instead of sigsuspend */
        if (g_sig_pipe[0] < 0) {
            int p[2], k;
            if (pipe(p) == 0) {
                for (k = 0; k < 2; k++) {
                    int high = fcntl(p[k], F_DUPFD, getdtablesize() > 64 ? getdtablesize() - 32 : 3);   /* out of the */
                    if (high >= 0) { close(p[k]); p[k] = high; }                                       /* guest's way */
                    fcntl(p[k], F_SETFL, O_NONBLOCK);
                    fcntl(p[k], F_SETFD, FD_CLOEXEC);
                }
                g_sig_pipe[0] = p[0];
                g_sig_pipe[1] = p[1];
            }
        }
        while (!(g_sig_pending & bits)) {
            char drain[64];
            struct pollfd pf;
            if (g_sig_pipe[0] < 0) { f_short_wait(); continue; }
            pf.fd = g_sig_pipe[0]; pf.events = POLLIN; pf.revents = 0;
            if (g_sig_pending & bits) break;
            f_poll(&pf, 1, 500);             /* the timeout only guards against a lost wake-up */
            while (read(g_sig_pipe[0], drain, sizeof drain) > 0) ;
        }
        return;
    }
    sigfillset(&all);
    sigprocmask(SIG_BLOCK, &all, &before);  /* do not lose a signal between check and wait */
    while (!(g_sig_pending & bits)) sigsuspend(&before);
    sigprocmask(SIG_SETMASK, &before, 0);
}

/* ---------- processes ----------
 * fork: the host forks (its libc prepares malloc and its own state); in the child only the calling thread exists
 * (thread_after_fork). i386 convention: edx = 1 and eax = own pid in the child, edx = 0 and eax = pid of the child
 * in the parent. vfork is a fork: the child shares no memory, which programs that only exec or _exit afterwards
 * cannot tell.
 * execve: a program that runs on PowerPC (thin ppc, fat with a ppc part, scripts, everything unknown) is started
 * by the kernel itself; an Intel-only program (thin i386, fat without ppc) runs through rosetta again, with the
 * caller's argv[0] passed by -0. The host signal mask becomes the guest mask, so the new program inherits it as it
 * would natively; when that program is rosetta, sys_init turns it back into the guest mask. */
static char g_self[PATH_MAX];               /* this rosetta binary (absolute) */

void sys_init(void)
{
    char exe[PATH_MAX];
    uint32_t n = sizeof exe;
    sigset_t m, none;
    int sig;
    if (_NSGetExecutablePath(exe, &n) != 0 || !realpath(exe, g_self)) g_self[0] = 0;
    for (sig = 1; sig < 32; sig++) {        /* ignored by the parent: the guest sees SIG_IGN (nohup, background jobs) */
        struct sigaction sa;
        if (!sig_own(sig) && sigaction(sig, 0, &sa) == 0 && sa.sa_handler == SIG_IGN) g_handler[sig] = 1;
    }
    sigprocmask(SIG_BLOCK, 0, &m);
    for (sig = 1; sig < 32; sig++) if (sigismember(&m, sig)) g_sig_mask |= SIG_BIT(sig);
    g_sig_mask &= ~SIG_NEVER;
    sig_sync();                              /* catch the blocked ones before the host unblocks them */
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, 0);
}

static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* 1: the file is an Intel program that PowerPC cannot run (CPU types: 7 i386, 18 ppc) */
static int exec_intel(const char *path)
{
    unsigned char h[4096];
    int fd = open(path, O_RDONLY), n, ppc = 0, intel = 0;
    uint32_t nfat, k;
    if (fd < 0) return 0;                    /* the kernel reports the error */
    n = (int)read(fd, h, sizeof h);
    close(fd);
    if (n < 8) return 0;
    if (be32(h) == 0xcefaedfeu) return h[4] == 7 && !h[5] && !h[6] && !h[7];   /* thin, little-endian header */
    if (be32(h) != 0xcafebabeu) return 0;
    nfat = be32(h + 4);
    if (nfat == 0 || nfat > (uint32_t)(n - 8) / 20) return 0;               /* also Java class files */
    for (k = 0; k < nfat; k++) {
        uint32_t cpu = be32(h + 8 + 20 * k);
        if (cpu == 18) ppc = 1;
        if (cpu == 7) intel = 1;
    }
    return intel && !ppc;
}

/* execve with guest argv/envp (arrays of guest pointers) and the signal mask the new program starts with; returns
   only on failure (errno) */
static int proc_exec(const char *path, uint32_t gargv, uint32_t genv, uint32_t mask)
{
    uint32_t na = 0, ne = 0, i, k = 0;
    char **av, **ev;
    sigset_t m, before;
    int sig, e, watchdog, intel = exec_intel(path);
    if (intel && !g_self[0]) return EBADARCH;
    while (gargv && ld32(gargv + 4 * na)) na++;
    while (genv && ld32(genv + 4 * ne)) ne++;
    av = malloc((na + 6) * sizeof *av);
    ev = malloc((ne + 1) * sizeof *ev);
    if (!av || !ev) { free(av); free(ev); return ENOMEM; }
    if (intel) {
        av[k++] = g_self;
        av[k++] = (char *)"-0";
        av[k++] = na ? (char *)(uintptr_t)ld32(gargv) : (char *)path;
        av[k++] = (char *)"--";
        av[k++] = (char *)path;
        for (i = 1; i < na; i++) av[k++] = (char *)(uintptr_t)ld32(gargv + 4 * i);
    } else
        for (i = 0; i < na; i++) av[k++] = (char *)(uintptr_t)ld32(gargv + 4 * i);
    av[k] = 0;
    for (i = 0; i < ne; i++) ev[i] = (char *)(uintptr_t)ld32(genv + 4 * i);
    ev[ne] = 0;
    sigemptyset(&m);
    for (sig = 1; sig < 32; sig++) if ((mask & SIG_BIT(sig)) && !sig_own(sig)) sigaddset(&m, sig);
    fflush(0);
    watchdog = thread_watchdog_stop();       /* the kernel refuses execve while other threads exist */
    sigprocmask(SIG_SETMASK, &m, &before);
    execve(intel ? g_self : path, av, ev);
    e = errno;
    sigprocmask(SIG_SETMASK, &before, 0);
    if (watchdog) thread_watchdog_start();
    if (g_log) {
        fprintf(stderr, "[execve] %s%s, %u arguments, %u environment: %s\n", intel ? "through rosetta: " : "", path, na, ne,
                strerror(e));
        for (i = 0; av[i]; i++) fprintf(stderr, "[execve]   argv[%u] %s\n", i, av[i]);
        for (i = 0; ev[i]; i++) fprintf(stderr, "[execve]   env %s\n", ev[i]);
    }
    free(av);
    free(ev);
    return e;
}

static pid_t proc_fork(Cpu *c)
{
    pid_t p;
    fflush(0);
    p = fork();
    if (p == 0) {
        thread_after_fork(c, 1);
        g_sig_pending = 0;                   /* a new process has no pending signals */
        if (g_sig_pipe[0] >= 0) {            /* the wake-up pipe belongs to the parent */
            close(g_sig_pipe[0]); close(g_sig_pipe[1]);
            g_sig_pipe[0] = g_sig_pipe[1] = -1;
        }
    }
    return p;
}

/* posix_spawn (10.5): descriptor {attr size, attr, file actions size, file actions, port actions size, port
   actions}; attr: flags (16 bit) @0, sigdefault @4, sigmask @8, pgroup @12; file actions: count @4, actions from @8,
   1040 bytes each: type @0, fd @4, open flags or dup2 target @8, mode (16 bit) @12, path @14. */
#define PSA_RESETIDS 0x01
#define PSA_SETPGROUP 0x02
#define PSA_SETSIGDEF 0x04
#define PSA_SETSIGMASK 0x08
#define PSA_SETEXEC 0x40
#define PSFA_OPEN 0
#define PSFA_CLOSE 1
#define PSFA_DUP2 2
#define PSFA_SIZE 1040u

/* in the new process: file actions and attributes; *report is the fd of the error pipe (moved out of the way if
   an action would hit it); returns errno */
static int spawn_setup(uint32_t attr, uint32_t acts, uint32_t *mask, int *report)
{
    uint32_t k, n = acts ? ld32(acts + 4) : 0;
    int top = -1;
    for (k = 0; k < n; k++) {
        uint32_t a = acts + 8 + PSFA_SIZE * k;
        int fd = (int)ld32(a + 4), to = ld32(a) == PSFA_DUP2 ? (int)ld32(a + 8) : fd;
        if (fd > top) top = fd;
        if (to > top) top = to;
    }
    if (report && *report >= 0 && *report <= top) {
        int moved = fcntl(*report, F_DUPFD, top + 1);
        if (moved >= 0) { fcntl(moved, F_SETFD, FD_CLOEXEC); close(*report); *report = moved; }
    }
    for (k = 0; k < n; k++) {
        uint32_t a = acts + 8 + PSFA_SIZE * k, type = ld32(a);
        int fd = (int)ld32(a + 4);
        if (type == PSFA_OPEN) {
            int t = open((const char *)(uintptr_t)(a + 14), (int)ld32(a + 8), (int)ld16(a + 12));
            if (t < 0) return errno;
            if (t != fd) {
                int e = dup2(t, fd) < 0 ? errno : 0;
                close(t);
                if (e) return e;
            }
        } else if (type == PSFA_CLOSE) {
            if (close(fd) < 0) return errno;
        } else if (type == PSFA_DUP2) {
            if (dup2(fd, (int)ld32(a + 8)) < 0) return errno;
        } else return EINVAL;
    }
    if (attr) {
        uint32_t fl = ld16(attr);
        if (fl & PSA_SETSIGMASK) *mask = ld32(attr + 8) & ~SIG_NEVER;
        if (fl & PSA_SETSIGDEF) {
            uint32_t d = ld32(attr + 4);
            int sig;
            for (sig = 1; sig < 32; sig++)
                if ((d & SIG_BIT(sig)) && !sig_own(sig)) signal(sig, SIG_DFL);
        }
        if ((fl & PSA_SETPGROUP) && setpgid(0, (pid_t)ld32(attr + 12)) < 0) return errno;
        if ((fl & PSA_RESETIDS) && (setgid(getgid()) < 0 || setuid(getuid()) < 0)) return errno;
    }
    return 0;
}

/* posix_spawn(pid *, path, desc *, argv, envp): fork, set up the child, execve as above; a pipe with close-on-exec
   reports a failed exec to the parent, which then returns its errno like the kernel */
static int proc_spawn(uint32_t pidp, const char *path, uint32_t desc, uint32_t gargv, uint32_t genv)
{
    uint32_t attr = 0, acts = 0, mask = g_sig_mask;
    int pfd[2], e = 0, st;
    pid_t p;
    ssize_t got;
    if (desc) {
        if (ld32(desc)) attr = ld32(desc + 4);
        if (ld32(desc + 8)) acts = ld32(desc + 12);
        if (ld32(desc + 16) && g_log) fprintf(stderr, "[posix_spawn] port actions ignored\n");
    }
    if (attr && (ld16(attr) & PSA_SETEXEC)) {   /* exec in this process */
        e = spawn_setup(attr, acts, &mask, 0);
        return e ? e : proc_exec(path, gargv, genv, mask);
    }
    if (pipe(pfd) < 0) return errno;
    fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pfd[1], F_SETFD, FD_CLOEXEC);
    fflush(0);
    p = fork();
    if (p < 0) { e = errno; close(pfd[0]); close(pfd[1]); return e; }
    if (p == 0) {                            /* child: no guest code runs here */
        int w = pfd[1];
        thread_after_fork(g_thread_cur, 0);  /* only this thread exists (proc_exec must not join the watchdog) */
        close(pfd[0]);
        e = spawn_setup(attr, acts, &mask, &w);
        if (!e) e = proc_exec(path, gargv, genv, mask);
        while (write(w, &e, sizeof e) < 0 && errno == EINTR) ;
        _exit(127);
    }
    close(pfd[1]);
    do got = f_read(pfd[0], &e, sizeof e); while (got < 0 && errno == EINTR);
    close(pfd[0]);
    if (got == (ssize_t)sizeof e && e) {
        f_waitpid(p, &st);
        return e;
    }
    if (pidp) st32(pidp, (uint32_t)p);
    return 0;
}

/* ---------- Intel versions of the system libraries (stage 4) ----------
 * On the PowerPC system the frameworks exist only for PowerPC; the i386 versions from Apple's Combo Update are
 * under $ROSETTA_INTEL (default <rosetta>/../intel/root) with the same paths. If the guest reads or checks a file
 * under /System/Library, /usr/lib or /Library that exists there, it gets that file; directories, resources and
 * everything else come from the system. Exceptions, so that programs that read libraries as data (file, nm, lipo,
 * backup and copy programs) see the real files: a Mach-O file that the system has with an i386 part itself is only
 * redirected for dyld (the prebound copy loads faster), and writing never goes to the root.
 * ROSETTA_INTEL=- switches this off. */
static char g_intel[PATH_MAX];
static int g_intel_on = -1;
static unsigned long g_st_redirected;
uint32_t g_dyld_from, g_dyld_to;            /* main.c: where dyld is (calls from there are the loader's) */
static uint32_t g_call_eip;                 /* eip of the current system call */

static const char *gpath_as(uint32_t a, int loader);
#define gpath(a) gpath_as((a), g_call_eip - g_dyld_from < g_dyld_to - g_dyld_from)
const char *sys_guest_path(const char *p) { return gpath_as((uint32_t)(uintptr_t)p, 1); }

/* remembered answers (dyld queries the same path several times with stat, open, ...): per path, whether it exists
   in the Intel root, and whether the system file has an i386 part itself (-1: not checked yet) */
#define GPATH_N 512
static struct { uint32_t h; char *path; char in_root, sys_i386; } g_gpath[GPATH_N];
static int g_ngpath;

/* 1: Mach-O file with an i386 part (thin i386 or fat with CPU type 7) */
static int file_has_i386(const char *path)
{
    unsigned char h[4096];
    int fd = open(path, O_RDONLY), n;
    uint32_t nfat, k;
    if (fd < 0) return 0;
    n = (int)read(fd, h, sizeof h);
    close(fd);
    if (n < 8) return 0;
    if (h[0] == 0xce && h[1] == 0xfa && h[2] == 0xed && h[3] == 0xfe) return h[4] == 7;
    if (!(h[0] == 0xca && h[1] == 0xfe && h[2] == 0xba && h[3] == 0xbe)) return 0;
    nfat = (uint32_t)h[4] << 24 | (uint32_t)h[5] << 16 | (uint32_t)h[6] << 8 | h[7];
    if (nfat == 0 || nfat > (uint32_t)(n - 8) / 20) return 0;
    for (k = 0; k < nfat; k++) if (!memcmp(h + 8 + 20 * k, "\0\0\0\7", 4)) return 1;
    return 0;
}

/* open for the guest: reading may be redirected, writing never */
static const char *gpath_open(uint32_t a, int flags)
{
    return (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) ? (const char *)(uintptr_t)a : gpath(a);
}

static const char *gpath_as(uint32_t a, int loader)
{
    static char fresh[2][1200];
    static int which;
    const char *p = (const char *)(uintptr_t)a;
    struct stat st;
    char *z;
    uint32_t h, i;
    const unsigned char *q;
    int in_root, sys_i386;
    if (g_intel_on < 0) {
        const char *e = getenv("ROSETTA_INTEL");
        char exe[1024], cand[1200];
        uint32_t en = sizeof exe;
        if (e) snprintf(g_intel, sizeof g_intel, "%s", e);
        else if (_NSGetExecutablePath(exe, &en) == 0 && strrchr(exe, '/')) {
            *strrchr(exe, '/') = 0;
            snprintf(cand, sizeof cand, "%s/../intel/root", exe);
            if (!realpath(cand, g_intel)) g_intel[0] = 0;
        }
        g_intel_on = g_intel[0] == '/' && stat(g_intel, &st) == 0 && S_ISDIR(st.st_mode);
    }
    if (!g_intel_on || !p || p[0] != '/') return p;
    if (strncmp(p, "/System/Library/", 16) && strncmp(p, "/usr/lib/", 9) && strncmp(p, "/Library/", 9)) return p;
    z = fresh[which ^= 1];                   /* two buffers: calls with two paths */
    if ((size_t)snprintf(z, sizeof fresh[0], "%s%s", g_intel, p) >= sizeof fresh[0]) return p;
    for (h = 2166136261u, q = (const unsigned char *)p; *q; q++) h = (h ^ *q) * 16777619u;
    for (i = h & (GPATH_N - 1); g_gpath[i].path; i = (i + 1) & (GPATH_N - 1))
        if (g_gpath[i].h == h && !strcmp(g_gpath[i].path, p)) break;
    if (g_gpath[i].path) { in_root = g_gpath[i].in_root; sys_i386 = g_gpath[i].sys_i386; }
    else {
        in_root = stat(z, &st) == 0 && !S_ISDIR(st.st_mode);
        sys_i386 = -1;
        if (g_ngpath < GPATH_N / 2 && (g_gpath[i].path = strdup(p)) != 0) {
            g_gpath[i].h = h; g_gpath[i].in_root = (char)in_root; g_gpath[i].sys_i386 = -1; g_ngpath++;
        } else i = GPATH_N;                   /* not remembered */
    }
    if (!in_root) return p;
    if (!loader) {                           /* data read by the program: the system file, if it would do */
        if (sys_i386 < 0) {
            sys_i386 = file_has_i386(p);
            if (i < GPATH_N) g_gpath[i].sys_i386 = (char)sys_i386;
        }
        if (sys_i386) return p;
    }
    g_st_redirected++;
    if (g_log) fprintf(stderr, "[intel] %s\n", z);
    return z;
}

/* /var/run/utmpx (who, w, users): records in host byte order; reading, the guest gets a converted copy (struct
   utmpx, 628 bytes: ut_user[256], ut_id[4], ut_line[32], ut_pid @292, ut_type (16) @296, ut_tv @300, ut_host[256]
   @308, ut_pad[16] @564) */
#define UTMPX_PATH "/var/run/utmpx"
#define UTMPX_SIZE 628
typedef char utmpx_size_check[sizeof(struct utmpx) == UTMPX_SIZE ? 1 : -1];

static int utmpx_open_copy(void)
{
    struct stat st;
    char tmpl[] = "/tmp/rosetta-utmpx.XXXXXX";
    uint8_t *b;
    int in = open(UTMPX_PATH, O_RDONLY), out = -1;
    ssize_t n;
    uint32_t o;
    if (in < 0) return -1;
    if (fstat(in, &st) < 0 || st.st_size > (16 << 20) || !(b = malloc((size_t)st.st_size + 1))) { close(in); return -1; }
    n = read(in, b, (size_t)st.st_size);
    close(in);
    for (o = 0; n > 0 && o + UTMPX_SIZE <= (uint32_t)n; o += UTMPX_SIZE) {
        uint32_t a = (uint32_t)(uintptr_t)(b + o);
        sw32(a + 292); sw16(a + 296); sw32(a + 300); sw32(a + 304); sw32_range(a, 564, 628);
    }
    if (n >= 0 && (out = mkstemp(tmpl)) >= 0) {
        unlink(tmpl);
        if (write(out, b, (size_t)n) != n || lseek(out, 0, SEEK_SET) != 0) { close(out); out = -1; }
    }
    free(b);
    return out;
}

/* ---------- system call switch ---------- */

static const char *sys_name(uint32_t n)
{
    switch (n) {
    case SYS_open: return "open"; case SYS_close: return "close"; case SYS_read: return "read"; case SYS_write: return "write";
    case SYS_mmap: return "mmap"; case SYS_munmap: return "munmap"; case SYS_mprotect: return "mprotect";
    case 188: return "stat"; case 189: return "fstat"; case 190: return "lstat";
    case 338: return "stat64"; case 339: return "fstat64"; case 340: return "lstat64";
    case SYS___sysctl: return "sysctl"; case SYS_ioctl: return "ioctl"; case SYS_fcntl: return "fcntl";
    case SYS_sigaction: return "sigaction"; case SYS_sigprocmask: return "sigprocmask";
    default: return "";
    }
}

/* ROSETTA_LOG=2: each log line starts with the seconds since the start and the host thread */
void sys_log_stamp(void)
{
    static uint64_t t0;
    static mach_timebase_info_data_t tb;
    uint64_t t = mach_absolute_time();
    if (!t0) { t0 = t; mach_timebase_info(&tb); }
    fprintf(stderr, "%9.3f %04x ", (double)(t - t0) * tb.numer / tb.denom / 1e9, (unsigned)(pthread_mach_thread_np(pthread_self()) & 0xffff));
}

/* *_nocancel versions (396..423) mapped to the normal calls */
static uint32_t no_nocancel(uint32_t n)
{
    static const uint16_t base[] = {
        SYS_read, SYS_write, SYS_open, SYS_close, SYS_wait4, SYS_recvmsg, SYS_sendmsg, SYS_recvfrom, SYS_accept,
        SYS_msync, SYS_fcntl, SYS_select, SYS_fsync, SYS_connect, SYS_sigsuspend, SYS_readv, SYS_writev, SYS_sendto,
        SYS_pread, SYS_pwrite, SYS_waitid, SYS_poll, 0, 0, 271 /* sem_wait */, 0, 330 /* __sigwait */,
        334 /* __semwait_signal */
    };
    if (n >= 396 && n <= 423 && base[n - 396]) return base[n - 396];
    return n;
}

void sys_bsd(Cpu *c, int sysenter)
{
    /* i386 libSystem puts the length of the arguments in the upper 16 bits */
    uint32_t n = no_nocancel(c->r[EAX] & 0xffff);
    if (!sysenter) g_arg_base = c->r[ESP];
    g_call_eip = c->eip;
    if (g_log) {
        if (g_log > 1) sys_log_stamp();
        fprintf(stderr, "[bsd %u %s] %08x %08x %08x %08x\n", n, sys_name(n), arg(0), arg(1), arg(2), arg(3));
    }
    switch (n) {
    case SYS_exit:
        c->finished = 1;
        c->end_code = (int)arg(0);
        return;
    case SYS_read: HOST(f_read((int)arg(0), PTR_ARG(1), arg(2))); return;
    case SYS_write: HOST(f_write((int)arg(0), PTR_ARG(1), arg(2))); return;
    case SYS_open: {
        const char *p = gpath_open(arg(0), (int)arg(1));
        int r = -1;
        if (!((int)arg(1) & O_ACCMODE) && !strcmp(p, UTMPX_PATH)) r = utmpx_open_copy();
        if (r < 0) r = open(p, (int)arg(1), (int)arg(2));
        if (r >= 0) jit_fd_off(r);
        HOST(r);
        return;
    }
    case SYS_close:
        if (sys_own_fd((int)arg(0))) { set_result(c, 0, 0); return; }   /* the translator's: the guest never had it */
        jit_fd_off((int)arg(0));
        HOST(close((int)arg(0)));
        return;
    case SYS_unlink: HOST(unlink((const char *)PTR_ARG(0))); return;
    case SYS_chdir: HOST(chdir((const char *)PTR_ARG(0))); return;
    case SYS_fchdir: HOST(fchdir((int)arg(0))); return;
    case SYS_getpid: set_result(c, (uint32_t)getpid(), 0); return;
    case SYS_getppid: set_result(c, (uint32_t)getppid(), 0); return;
    case SYS_getuid: set_result(c, getuid(), 0); return;
    case SYS_geteuid: set_result(c, geteuid(), 0); return;
    case SYS_getgid: set_result(c, getgid(), 0); return;
    case SYS_getegid: set_result(c, getegid(), 0); return;
    case SYS_issetugid: set_result(c, 0, 0); return;
    case SYS_getpgrp: set_result(c, (uint32_t)getpgrp(), 0); return;
    case SYS_access: HOST(access(gpath(arg(0)), (int)arg(1))); return;
    case SYS_dup: HOST(dup((int)arg(0))); return;
    case SYS_dup2:
        if (sys_own_fd((int)arg(0))) { set_result(c, 0, EBADF); return; }
        sys_own_fd_move((int)arg(1));       /* the guest wants this number: the translator's descriptor moves away */
        jit_fd_off((int)arg(1));
        HOST(dup2((int)arg(0), (int)arg(1)));
        return;
    case SYS_umask: set_result(c, umask((mode_t)arg(0)), 0); return;
    case SYS_getdtablesize: set_result(c, (uint32_t)getdtablesize(), 0); return;
    case SYS_readlink: HOST(readlink(gpath(arg(0)), PTR_ARG(1), arg(2))); return;
    case SYS_mkdir: HOST(mkdir((const char *)PTR_ARG(0), (mode_t)arg(1))); return;
    case SYS_rmdir: HOST(rmdir((const char *)PTR_ARG(0))); return;
    case SYS_rename: HOST(rename((const char *)PTR_ARG(0), (const char *)PTR_ARG(1))); return;
    case SYS_fsync: HOST(fsync((int)arg(0))); return;
    case SYS_pipe: {
        int p[2];
        if (pipe(p) < 0) { set_result(c, 0, errno); return; }
        c->r[EDX] = (uint32_t)p[1];
        set_result(c, (uint32_t)p[0], 0);
        return;
    }
    case SYS_pread: HOST(pread((int)arg(0), PTR_ARG(1), arg(2), (off_t)arg64(3))); return;
    case SYS_pwrite: HOST(pwrite((int)arg(0), PTR_ARG(1), arg(2), (off_t)arg64(3))); return;
    case SYS_lseek: {
        off_t r = lseek((int)arg(0), (off_t)arg64(1), (int)arg(3));
        if (r < 0) { set_result(c, 0, errno); return; }
        c->r[EDX] = (uint32_t)((uint64_t)r >> 32);
        set_result(c, (uint32_t)r, 0);
        return;
    }
    case SYS_mmap: {
        uint32_t a = arg(0), len = arg(1), flags = arg(3);
        int prot = (int)arg(2), shared;
        void *r;
        if (flags & MAP_FIXED) sys_mem_note(c, 'm', a, len);
        if ((flags & MAP_FIXED) && sys_host_range(c, a, len, "mmap")) { set_result(c, 0, EINVAL); return; }
        if ((flags & MAP_FIXED) && a < RESERVE_END && a >= 0x1000)
            vm_deallocate(mach_task_self(), a, len);         /* space in the guest reserve */
        if (g_shared_on < 0) g_shared_on = !getenv("ROSETTA_NO_SHARED");
        /* file read-only, private: map it shared (see above) */
        shared = g_shared_on && !(flags & MAP_ANON) && (flags & MAP_PRIVATE) && !(prot & PROT_WRITE) && (int)arg(4) >= 0 &&
                  g_nshared < SHARED_N;
        r = MAP_FAILED;
        if (shared) {
            r = mmap((void *)(uintptr_t)a, len, prot, (int)((flags & ~MAP_PRIVATE) | MAP_SHARED), (int)arg(4), (off_t)arg64(5));
            if (r != MAP_FAILED) {
                g_shared[g_nshared].start = (uint32_t)(uintptr_t)r;
                g_shared[g_nshared].end = (uint32_t)(uintptr_t)r + ((len + 4095) & ~4095u);
                g_shared[g_nshared++].prot = prot;
                g_st_shared++;
            }
        }
        if (r == MAP_FAILED) r = mmap((void *)(uintptr_t)a, len, prot, (int)flags, (int)arg(4), (off_t)arg64(5));
        if (r == MAP_FAILED) { set_result(c, 0, errno); return; }
        /* Code at this address may have changed; not when dyld places a library from the translation cache at its
           address (jit_mapping_expected) */
        if (flags & MAP_FIXED) jit_flush_schedule(a, jit_mapping_expected(a, len, (int)arg(4)) ? 0 : len);
        else jit_new_mapped((uint32_t)(uintptr_t)r, len);
        set_result(c, (uint32_t)(uintptr_t)r, 0);
        return;
    }
    case SYS_munmap:
        sys_mem_note(c, 'u', arg(0), arg(1));
        if (sys_host_range(c, arg(0), arg(1), "munmap")) { set_result(c, 0, EINVAL); return; }
        jit_flush_schedule(arg(0), arg(1)); shared_forget(arg(0), arg(1)); HOST(munmap(PTR_ARG(0), arg(1))); return;
    case SYS_mprotect:
        sys_mem_note(c, 'p', arg(0), arg(1));
        if (sys_host_range(c, arg(0), arg(1), "mprotect")) { set_result(c, 0, EINVAL); return; }
        jit_flush_schedule(arg(0), arg(1));
        if (arg(2) & PROT_WRITE) sys_make_private(arg(0), arg(1));
        HOST(mprotect(PTR_ARG(0), arg(1), (int)arg(2)));
        return;
    case SYS_madvise: HOST(madvise(PTR_ARG(0), arg(1), (int)arg(2))); return;
    case SYS_msync: HOST(msync(PTR_ARG(0), arg(1), (int)arg(2))); return;
    case 188: case 190: {                   /* stat, lstat */
        struct stat s;
        int r = n == 188 ? stat(gpath(arg(0)), &s) : lstat(gpath(arg(0)), &s);
        if (r < 0) { set_result(c, 0, errno); return; }
        stat_write(arg(1), &s);
        set_result(c, 0, 0);
        return;
    }
    case 189: {                             /* fstat */
        struct stat s;
        if (fstat((int)arg(0), &s) < 0) { set_result(c, 0, errno); return; }
        stat_write(arg(1), &s);
        set_result(c, 0, 0);
        return;
    }
    case 338: case 340: {                   /* stat64, lstat64 */
        struct stat64 s;
        int r = n == 338 ? stat64(gpath(arg(0)), &s) : lstat64(gpath(arg(0)), &s);
        if (r < 0) { set_result(c, 0, errno); return; }
        stat64_write(arg(1), &s);
        set_result(c, 0, 0);
        return;
    }
    case 339: {                             /* fstat64 */
        struct stat64 s;
        if (fstat64((int)arg(0), &s) < 0) { set_result(c, 0, errno); return; }
        stat64_write(arg(1), &s);
        set_result(c, 0, 0);
        return;
    }
    case SYS_statfs: case SYS_fstatfs: {
        struct statfs s;
        int r = n == SYS_statfs ? statfs((const char *)PTR_ARG(0), &s) : fstatfs((int)arg(0), &s);
        if (r < 0) { set_result(c, 0, errno); return; }
        statfs_write(arg(1), &s);
        set_result(c, 0, 0);
        return;
    }
    case 345: case 346: {                   /* statfs64, fstatfs64 */
        struct statfs64 s;
        int r = n == 345 ? statfs64((const char *)PTR_ARG(0), &s) : fstatfs64((int)arg(0), &s);
        if (r < 0) { set_result(c, 0, errno); return; }
        statfs64_write(arg(1), &s);
        set_result(c, 0, 0);
        return;
    }
    case 18: {                              /* getfsstat(buf, size, flags), old statfs (0x110 bytes) */
        uint32_t target = arg(0), count = arg(1) / 0x110, i;
        int r;
        struct statfs *s = count ? calloc(count, sizeof *s) : 0;
        r = getfsstat(s, (int)(count * sizeof *s), (int)arg(2));
        if (r < 0) { free(s); set_result(c, 0, errno); return; }
        for (i = 0; s && i < (uint32_t)r && i < count; i++) statfs_write(target + i * 0x110, &s[i]);
        free(s);
        set_result(c, (uint32_t)r, 0);
        return;
    }
    case 220: {                             /* getattrlist(path, list, buffer, size, options) */
        struct attrlist l;
        uint32_t size = arg(3);
        void *b = malloc(size ? size : 4);
        int r;
        attrlist_read(arg(1), &l);
        r = getattrlist(gpath(arg(0)), &l, b, size, (unsigned)arg(4));
        if (r < 0) { free(b); set_result(c, 0, errno); return; }
        memcpy(PTR_ARG(2), b, size);
        free(b);
        attr_swap(arg(2), size, &l);
        set_result(c, 0, 0);
        return;
    }
    case 222: {                             /* getdirentriesattr(fd, list, buffer, size, *count, *base, *state, opt) */
        struct attrlist l;
        uint32_t size = arg(3), i, o = 0;
        unsigned long count = ld32(arg(4)), base = 0, state = 0;     /* 10.5: unsigned long */
        uint8_t *b = malloc(size ? size : 4);
        int r;
        attrlist_read(arg(1), &l);
        r = getdirentriesattr((int)arg(0), &l, b, size, &count, &base, &state, (unsigned)arg(7));
        if (r < 0) { free(b); set_result(c, 0, errno); return; }
        memcpy(PTR_ARG(2), b, size);
        for (i = 0; i < count && o + 4 <= size; i++) {    /* entries one after another, each with the length in front */
            uint32_t lg = *(uint32_t *)(b + o);
            attr_swap(arg(2) + o, size - o, &l);
            if (!lg) break;
            o += (lg + 3) & ~3u;
        }
        free(b);
        st32(arg(4), (uint32_t)count);
        if (arg(5)) st32(arg(5), (uint32_t)base);
        if (arg(6)) st32(arg(6), (uint32_t)state);
        set_result(c, (uint32_t)r, 0);
        return;
    }
    case 347: {                             /* getfsstat64(buf, size, flags) */
        uint32_t target = arg(0), count = arg(1) / 0x878, i;
        int r;
        struct statfs64 *s = count ? calloc(count, sizeof *s) : 0;
        r = getfsstat64(s, (int)(count * sizeof *s), (int)arg(2));
        if (r < 0) { free(s); set_result(c, 0, errno); return; }
        for (i = 0; s && i < (uint32_t)r && i < count; i++) statfs64_write(target + i * 0x878, &s[i]);
        free(s);
        set_result(c, (uint32_t)r, 0);
        return;
    }
    case 341: case 342: case 343: {         /* stat64_extended, lstat64_extended, fstat64_extended */
        struct stat64 s;
        int r = n == 341 ? stat64((const char *)PTR_ARG(0), &s) : n == 342 ? lstat64((const char *)PTR_ARG(0), &s)
                                                                           : fstat64((int)arg(0), &s);
        if (r < 0) { set_result(c, 0, errno); return; }
        stat64_write(arg(1), &s);
        if (arg(3)) st32(arg(3), 0);        /* no ACL reported */
        set_result(c, 0, 0);
        return;
    }
    case 234: HOST(syscall(234, PTR_ARG(0), PTR_ARG(1), PTR_ARG(2), arg(3), arg(4), (int)arg(5))); return;   /* getxattr */
    case 235: HOST(syscall(235, (int)arg(0), PTR_ARG(1), PTR_ARG(2), arg(3), arg(4), (int)arg(5))); return; /* fgetxattr */
    case 236: HOST(syscall(236, PTR_ARG(0), PTR_ARG(1), PTR_ARG(2), arg(3), arg(4), (int)arg(5))); return;   /* setxattr */
    case 237: HOST(syscall(237, (int)arg(0), PTR_ARG(1), PTR_ARG(2), arg(3), arg(4), (int)arg(5))); return; /* fsetxattr */
    case 238: HOST(syscall(238, PTR_ARG(0), PTR_ARG(1), (int)arg(2))); return;                              /* removexattr */
    case 239: HOST(syscall(239, (int)arg(0), PTR_ARG(1), (int)arg(2))); return;                            /* fremovexattr */
    case 240: HOST(syscall(240, PTR_ARG(0), PTR_ARG(1), arg(2), (int)arg(3))); return;                      /* listxattr */
    case 241: HOST(syscall(241, (int)arg(0), PTR_ARG(1), arg(2), (int)arg(3))); return;                    /* flistxattr */
    case 266:                               /* shm_open(name, flags, mode) */
        HOST(syscall(266, PTR_ARG(0), (int)arg(1), (int)arg(2)));
        return;
    case 295:                               /* shared_region_map_file_np: no shared i386 region */
        set_result(c, 0, EINVAL);
        return;
    case SYS___sysctl: sys_sysctl(c); return;
    case SYS_gettimeofday: {                /* time in eax/edx, like the kernel */
        struct timeval t;
        gettimeofday(&t, 0);
        c->r[EDX] = (uint32_t)t.tv_usec;
        set_result(c, (uint32_t)t.tv_sec, 0);
        return;
    }
    case SYS_getrlimit: {
        struct rlimit r;
        if (getrlimit((int)arg(0), &r) < 0) { set_result(c, 0, errno); return; }
        st64(arg(1), r.rlim_cur); st64(arg(1) + 8, r.rlim_max);
        set_result(c, 0, 0);
        return;
    }
    case SYS_setrlimit: {
        struct rlimit r;
        r.rlim_cur = ld64(arg(1)); r.rlim_max = ld64(arg(1) + 8);
        HOST(setrlimit((int)arg(0), &r));
        return;
    }
    case SYS_getrusage: {
        struct rusage r;
        uint32_t i, *w = (uint32_t *)&r;
        if (getrusage((int)arg(0), &r) < 0) { set_result(c, 0, errno); return; }
        for (i = 0; i < sizeof r / 4; i++) st32(arg(1) + 4 * i, w[i]);
        set_result(c, 0, 0);
        return;
    }
    case SYS_readv: case SYS_writev: {
        uint32_t cnt = arg(2), i;
        struct iovec v[64];
        if (cnt > 64) { set_result(c, 0, EINVAL); return; }
        for (i = 0; i < cnt; i++) {
            v[i].iov_base = (void *)(uintptr_t)ld32(arg(1) + 8 * i);
            v[i].iov_len = ld32(arg(1) + 8 * i + 4);
        }
        if (n == SYS_readv) HOST(f_readv((int)arg(0), v, (int)cnt)); else HOST(f_writev((int)arg(0), v, (int)cnt));
        return;
    }
    case SYS_pathconf: HOST(pathconf(gpath(arg(0)), (int)arg(1))); return;
    case SYS_fpathconf: HOST(fpathconf((int)arg(0), (int)arg(1))); return;
    /* Sockets: addresses (sockaddr_un, sockaddr_in) are byte-identical on both sides (length byte, family byte, port
       and address in network byte order); swapped are length pointers, option values, msghdr/iovec and control data */
    case SYS_socket: HOST(socket((int)arg(0), (int)arg(1), (int)arg(2))); return;
    case SYS_connect: HOST(f_connect((int)arg(0), (const struct sockaddr *)PTR_ARG(1), (socklen_t)arg(2))); return;
    case SYS_bind: HOST(bind((int)arg(0), (const struct sockaddr *)PTR_ARG(1), (socklen_t)arg(2))); return;
    case SYS_listen: HOST(listen((int)arg(0), (int)arg(1))); return;
    case SYS_shutdown: HOST(shutdown((int)arg(0), (int)arg(1))); return;
    case SYS_accept: case SYS_getsockname: case SYS_getpeername: {
        socklen_t l = arg(2) ? (socklen_t)ld32(arg(2)) : 0;
        struct sockaddr *sa = (struct sockaddr *)PTR_ARG(1);
        int r = n == SYS_accept ? f_accept((int)arg(0), sa, arg(2) ? &l : 0) :
                n == SYS_getsockname ? getsockname((int)arg(0), sa, &l) : getpeername((int)arg(0), sa, &l);
        if (r < 0) { set_result(c, 0, errno); return; }
        if (arg(2)) st32(arg(2), (uint32_t)l);
        set_result(c, (uint32_t)r, 0);
        return;
    }
    case SYS_socketpair: {
        int sv[2];
        if (socketpair((int)arg(0), (int)arg(1), (int)arg(2), sv) < 0) { set_result(c, 0, errno); return; }
        st32(arg(3), (uint32_t)sv[0]); st32(arg(3) + 4, (uint32_t)sv[1]);
        set_result(c, 0, 0);
        return;
    }
    case SYS_setsockopt: case SYS_getsockopt: {   /* swap values made of whole words (int, timeval, linger) */
        uint32_t val = arg(3), k;
        uint8_t buf[256];
        socklen_t l = n == SYS_setsockopt ? (socklen_t)arg(4) : (arg(4) ? (socklen_t)ld32(arg(4)) : 0);
        int r;
        if (l > sizeof buf) { set_result(c, 0, EINVAL); return; }
        if (n == SYS_setsockopt) {
            if (val) memcpy(buf, (const void *)(uintptr_t)val, l);
            if (l % 4 == 0 && l <= 16) for (k = 0; k < l; k += 4) { uint32_t w = ld32(val + k); memcpy(buf + k, &w, 4); }
            HOST(setsockopt((int)arg(0), (int)arg(1), (int)arg(2), val ? buf : 0, l));
            return;
        }
        r = getsockopt((int)arg(0), (int)arg(1), (int)arg(2), buf, &l);
        if (r < 0) { set_result(c, 0, errno); return; }
        if (val) {
            if (l % 4 == 0 && l <= 16) for (k = 0; k < l; k += 4) { uint32_t w; memcpy(&w, buf + k, 4); st32(val + k, w); }
            else memcpy((void *)(uintptr_t)val, buf, l);
        }
        if (arg(4)) st32(arg(4), (uint32_t)l);
        set_result(c, 0, 0);
        return;
    }
    case SYS_sendto:
        HOST(f_sendto((int)arg(0), PTR_ARG(1), arg(2), (int)arg(3), arg(4) ? (const struct sockaddr *)PTR_ARG(4) : 0, (socklen_t)arg(5)));
        return;
    case SYS_recvfrom: {
        socklen_t l = arg(5) ? (socklen_t)ld32(arg(5)) : 0;
        long r = f_recvfrom((int)arg(0), PTR_ARG(1), arg(2), (int)arg(3), arg(4) ? (struct sockaddr *)PTR_ARG(4) : 0, arg(5) ? &l : 0);
        if (r < 0) { set_result(c, 0, errno); return; }
        if (arg(5)) st32(arg(5), (uint32_t)l);
        set_result(c, (uint32_t)r, 0);
        return;
    }
    case SYS_sendmsg: case SYS_recvmsg: {
        /* msghdr (guest, 28 bytes): name, namelen, iov, iovlen, control, controllen, flags; cmsghdr: len, level, type,
           data (file descriptors for SCM_RIGHTS) */
        uint32_t a = arg(1), gv = ld32(a + 8), gc = ld32(a + 16), cl = ld32(a + 20), k;
        struct msghdr m;
        struct iovec v[64];
        uint8_t ctl[1024];
        long r;
        memset(&m, 0, sizeof m);
        m.msg_name = (void *)(uintptr_t)ld32(a);
        m.msg_namelen = ld32(a + 4);
        m.msg_iovlen = (int)ld32(a + 12);
        if (m.msg_iovlen > 64 || cl > sizeof ctl) { set_result(c, 0, EINVAL); return; }
        for (k = 0; k < (uint32_t)m.msg_iovlen; k++) {
            v[k].iov_base = (void *)(uintptr_t)ld32(gv + 8 * k);
            v[k].iov_len = ld32(gv + 8 * k + 4);
        }
        m.msg_iov = v;
        m.msg_control = gc && cl ? ctl : 0;
        m.msg_controllen = gc ? cl : 0;
        m.msg_flags = (int)ld32(a + 24);
        if (n == SYS_sendmsg && gc && cl) {     /* control data guest -> host */
            uint32_t o = 0;
            memcpy(ctl, (const void *)(uintptr_t)gc, cl);
            while (o + 12 <= cl) {
                uint32_t len = ld32(gc + o), lev = ld32(gc + o + 4), type = ld32(gc + o + 8), j;
                memcpy(ctl + o, &len, 4); memcpy(ctl + o + 4, &lev, 4); memcpy(ctl + o + 8, &type, 4);
                if (lev == SOL_SOCKET && type == SCM_RIGHTS)
                    for (j = 12; j + 4 <= len && o + j + 4 <= cl; j += 4) { uint32_t fd = ld32(gc + o + j); memcpy(ctl + o + j, &fd, 4); }
                if (len < 12) break;
                o += (len + 3) & ~3u;
            }
        }
        r = n == SYS_sendmsg ? f_sendmsg((int)arg(0), &m, (int)arg(2)) : f_recvmsg((int)arg(0), &m, (int)arg(2));
        if (r < 0) { set_result(c, 0, errno); return; }
        if (n == SYS_recvmsg) {
            if (gc && m.msg_control && m.msg_controllen) {   /* control data host -> guest */
                uint32_t o = 0, ml = m.msg_controllen;
                memcpy((void *)(uintptr_t)gc, ctl, ml);
                while (o + 12 <= ml) {
                    uint32_t len, lev, type, j;
                    memcpy(&len, ctl + o, 4); memcpy(&lev, ctl + o + 4, 4); memcpy(&type, ctl + o + 8, 4);
                    st32(gc + o, len); st32(gc + o + 4, lev); st32(gc + o + 8, type);
                    if (lev == SOL_SOCKET && type == SCM_RIGHTS)
                        for (j = 12; j + 4 <= len && o + j + 4 <= ml; j += 4) { uint32_t fd; memcpy(&fd, ctl + o + j, 4); st32(gc + o + j, fd); }
                    if (len < 12) break;
                    o += (len + 3) & ~3u;
                }
            }
            st32(a + 4, m.msg_namelen);
            st32(a + 20, gc ? m.msg_controllen : 0);
            st32(a + 24, (uint32_t)m.msg_flags);
        }
        set_result(c, (uint32_t)r, 0);
        return;
    }
    case SYS_select: {                      /* fd_set: swap 32-bit words (bit n%32 in word n/32) */
        int nfds = (int)arg(0), k, j, w, r;
        fd_set f[3];
        struct timeval tv;
        uint32_t pz[3];
        pz[0] = arg(1); pz[1] = arg(2); pz[2] = arg(3);
        if (nfds < 0 || nfds > FD_SETSIZE) { set_result(c, 0, EINVAL); return; }
        w = (nfds + 31) / 32;
        for (k = 0; k < 3; k++)
            if (pz[k]) { FD_ZERO(&f[k]); for (j = 0; j < w; j++) ((uint32_t *)(void *)&f[k])[j] = ld32(pz[k] + 4 * (uint32_t)j); }
        if (arg(4)) { tv.tv_sec = (time_t)ld32(arg(4)); tv.tv_usec = (suseconds_t)ld32(arg(4) + 4); }
        r = f_select(nfds, pz[0] ? &f[0] : 0, pz[1] ? &f[1] : 0, pz[2] ? &f[2] : 0, arg(4) ? &tv : 0);
        if (r < 0) { set_result(c, 0, errno); return; }
        for (k = 0; k < 3; k++)
            if (pz[k]) for (j = 0; j < w; j++) st32(pz[k] + 4 * (uint32_t)j, ((uint32_t *)(void *)&f[k])[j]);
        set_result(c, (uint32_t)r, 0);
        return;
    }
    case SYS_poll: {                        /* pollfd: fd (32), events (16), revents (16) */
        uint32_t a = arg(0), nf = arg(1), k;
        struct pollfd pf[256];
        int r;
        if (nf > 256) { set_result(c, 0, EINVAL); return; }
        for (k = 0; k < nf; k++) { pf[k].fd = (int)ld32(a + 8 * k); pf[k].events = (short)ld16(a + 8 * k + 4); pf[k].revents = 0; }
        r = f_poll(pf, nf, (int)arg(2));
        if (r < 0) { set_result(c, 0, errno); return; }
        for (k = 0; k < nf; k++) st16(a + 8 * k + 6, (uint16_t)pf[k].revents);
        set_result(c, (uint32_t)r, 0);
        return;
    }
    case SYS_fcntl: {
        int cmd = (int)arg(1);
        switch (cmd) {
        case F_GETPATH: HOST(fcntl((int)arg(0), cmd, PTR_ARG(2))); return;
        case F_GETLK: case F_SETLK: case F_SETLKW: {
            /* struct flock (i386 and PowerPC): l_start (64) @0, l_len (64) @8, l_pid @16, l_type (16) @20,
               l_whence (16) @22; F_SETLKW waits without the thread lock */
            struct flock l;
            uint32_t a = arg(2);
            int r;
            l.l_start = (off_t)ld64(a); l.l_len = (off_t)ld64(a + 8); l.l_pid = (pid_t)ld32(a + 16);
            l.l_type = (short)ld16(a + 20); l.l_whence = (short)ld16(a + 22);
            r = cmd == F_SETLKW ? f_fcntl_ptr((int)arg(0), cmd, &l) : fcntl((int)arg(0), cmd, &l);
            if (r < 0) { set_result(c, 0, errno); return; }
            if (cmd == F_GETLK) {
                st64(a, (uint64_t)l.l_start); st64(a + 8, (uint64_t)l.l_len); st32(a + 16, (uint32_t)l.l_pid);
                st16(a + 20, (uint16_t)l.l_type); st16(a + 22, (uint16_t)l.l_whence);
            }
            set_result(c, (uint32_t)r, 0);
            return;
        }
        case F_PREALLOCATE: {               /* fstore_t: flags @0, posmode @4, offset @8, length @16, bytesalloc @24 */
            fstore_t f;
            uint32_t a = arg(2);
            int r;
            f.fst_flags = ld32(a); f.fst_posmode = (int)ld32(a + 4);
            f.fst_offset = (off_t)ld64(a + 8); f.fst_length = (off_t)ld64(a + 16); f.fst_bytesalloc = 0;
            r = f_fcntl_ptr((int)arg(0), cmd, &f);
            if (r < 0) { set_result(c, 0, errno); return; }
            st64(a + 24, (uint64_t)f.fst_bytesalloc);
            set_result(c, (uint32_t)r, 0);
            return;
        }
        case F_SETSIZE: {                   /* off_t * */
            off_t s = (off_t)ld64(arg(2));
            HOST(fcntl((int)arg(0), cmd, &s));
            return;
        }
        case F_RDADVISE: {                  /* struct radvisory: offset (64) @0, count @8 */
            struct radvisory ra;
            ra.ra_offset = (off_t)ld64(arg(2)); ra.ra_count = (int)ld32(arg(2) + 8);
            HOST(fcntl((int)arg(0), cmd, &ra));
            return;
        }
        case F_LOG2PHYS: {                  /* struct log2phys: flags @0, contigbytes (64) @4, devoffset (64) @12 */
            struct log2phys l;
            int r;
            memset(&l, 0, sizeof l);
            r = fcntl((int)arg(0), cmd, &l);
            if (r < 0) { set_result(c, 0, errno); return; }
            st32(arg(2), l.l2p_flags); st64(arg(2) + 4, (uint64_t)l.l2p_contigbytes); st64(arg(2) + 12, (uint64_t)l.l2p_devoffset);
            set_result(c, (uint32_t)r, 0);
            return;
        }
        default: HOST(fcntl((int)arg(0), cmd, (int)arg(2))); return;
        }
    }
    case SYS_ioctl: {
        unsigned long req = arg(1);
        if (req == TIOCGETA) {
            struct termios t;
            uint32_t a = arg(2);
            if (ioctl((int)arg(0), req, &t) < 0) { set_result(c, 0, errno); return; }
            st32(a, (uint32_t)t.c_iflag); st32(a + 4, (uint32_t)t.c_oflag); st32(a + 8, (uint32_t)t.c_cflag);
            st32(a + 12, (uint32_t)t.c_lflag); memcpy((void *)(uintptr_t)(a + 16), t.c_cc, 20);
            st32(a + 0x24, (uint32_t)t.c_ispeed); st32(a + 0x28, (uint32_t)t.c_ospeed);
            set_result(c, 0, 0);
            return;
        }
        if (req == TIOCGWINSZ) {
            struct winsize w;
            uint32_t a = arg(2);
            if (ioctl((int)arg(0), req, &w) < 0) { set_result(c, 0, errno); return; }
            st16(a, w.ws_row); st16(a + 2, w.ws_col); st16(a + 4, w.ws_xpixel); st16(a + 6, w.ws_ypixel);
            set_result(c, 0, 0);
            return;
        }
        if (req == TIOCSETA || req == TIOCSETAW || req == TIOCSETAF) {   /* tcsetattr: same layout as TIOCGETA */
            struct termios t;
            uint32_t a = arg(2);
            t.c_iflag = ld32(a); t.c_oflag = ld32(a + 4); t.c_cflag = ld32(a + 8); t.c_lflag = ld32(a + 12);
            memcpy(t.c_cc, (const void *)(uintptr_t)(a + 16), 20);
            t.c_ispeed = ld32(a + 0x24); t.c_ospeed = ld32(a + 0x28);
            HOST(f_ioctl((int)arg(0), req, &t));     /* TIOCSETAW/F wait until the output is written */
            return;
        }
        if (req == TIOCSWINSZ) {
            struct winsize w;
            uint32_t a = arg(2);
            w.ws_row = ld16(a); w.ws_col = ld16(a + 2); w.ws_xpixel = ld16(a + 4); w.ws_ypixel = ld16(a + 6);
            HOST(ioctl((int)arg(0), req, &w));
            return;
        }
        if (req == FIONREAD || req == FIODTYPE || req == FIOGETOWN || req == TIOCGPGRP || req == TIOCOUTQ ||
            req == TIOCMGET || req == TIOCGETD) {  /* int out */
            int v;
            if (ioctl((int)arg(0), req, &v) < 0) { set_result(c, 0, errno); return; }
            st32(arg(2), (uint32_t)v);
            set_result(c, 0, 0);
            return;
        }
        if (req == FIONBIO || req == FIOASYNC || req == FIOSETOWN || req == TIOCSPGRP || req == TIOCFLUSH ||
            req == TIOCMSET || req == TIOCMBIS || req == TIOCMBIC || req == TIOCSETD) {     /* int in */
            int v = (int)ld32(arg(2));
            HOST(ioctl((int)arg(0), req, &v));
            return;
        }
#ifdef TIOCPTYGNAME
        if (req == TIOCPTYGNAME) { HOST(ioctl((int)arg(0), req, PTR_ARG(2))); return; }     /* char[128] */
#endif
        if (req == TIOCSTI) { HOST(ioctl((int)arg(0), req, PTR_ARG(2))); return; }          /* char */
        if (req == 0x80086804) { set_result(c, 0, 0); return; }     /* dtrace helper: do not register the guest DOF */
        if ((req & IOC_DIRMASK) == IOC_VOID) { HOST(ioctl((int)arg(0), req)); return; }   /* no data: as it is */
        if ((req & IOC_DIRMASK) == IOC_OUT && IOCPARM_LEN(req) <= 1024) {
            /* only output, layout unknown: ask the host into a scratch buffer; the usual answer for a descriptor of
               another kind (MTIOCGET on a file: ENOTTY) needs no conversion */
            uint8_t scratch[1024];
            if (ioctl((int)arg(0), req, scratch) < 0) { set_result(c, 0, errno); return; }
        }
        {
            static unsigned long reported[16];
            int k;
            for (k = 0; k < 16 && reported[k] && reported[k] != req; k++) ;
            if (k < 16 && !reported[k]) { reported[k] = req; fprintf(stderr, "rosetta: ioctl %08lx not implemented yet\n", req); }
        }
        set_result(c, 0, ENOTTY);
        return;
    }
    case 196: {                             /* getdirentries(fd, buf, n, long *basep) */
        long base = 0;
        int r = (int)syscall(196, (int)arg(0), PTR_ARG(1), (int)arg(2), &base);
        if (r < 0) { set_result(c, 0, errno); return; }
        dirent_convert(arg(1), r);
        if (arg(3)) st32(arg(3), (uint32_t)base);
        set_result(c, (uint32_t)r, 0);
        return;
    }
    case 344: {                             /* getdirentries64(fd, buf, n, off_t *basep) */
        off_t base = 0;
        long r = (long)syscall(344, (int)arg(0), PTR_ARG(1), (int)arg(2), &base);
        if (r < 0) { set_result(c, 0, errno); return; }
        dirent64_convert(arg(1), r);
        if (arg(3)) st64(arg(3), (uint64_t)base);
        set_result(c, (uint32_t)r, 0);
        return;
    }
    case SYS_sigaction: {
        /* new: struct __sigaction (handler, trampoline, mask, flags); old: struct sigaction (handler, mask, flags) */
        uint32_t sig = arg(0), fresh = arg(1), prev = arg(2);
        if (sig == 0 || sig >= 32 || (fresh && (sig == SIGKILL || sig == SIGSTOP))) { set_result(c, 0, EINVAL); return; }
        if (prev) { st32(prev, g_handler[sig]); st32(prev + 4, g_handler_mask[sig]); st32(prev + 8, g_handler_flags[sig]); }
        if (fresh) {
            g_handler[sig] = ld32(fresh);
            if (ld32(fresh + 4)) g_tramp = ld32(fresh + 4);
            g_handler_mask[sig] = ld32(fresh + 8);
            g_handler_flags[sig] = ld32(fresh + 12);
            sig_host_set((int)sig);
            if (g_handler[sig] > 1) thread_watchdog_start();  /* handler: deliver the signal also in compute loops */
        }
        set_result(c, 0, 0);
        return;
    }
    case SYS_sigprocmask: case 329: {       /* sigprocmask, __pthread_sigmask(how, set, old): one mask for all threads */
        uint32_t how = arg(0), fresh = arg(1), prev = arg(2), old = g_sig_mask;
        if (fresh) {
            uint32_t m = ld32(fresh);
            if (how == SIG_BLOCK) g_sig_mask |= m; else if (how == SIG_UNBLOCK) g_sig_mask &= ~m;
            else if (how == SIG_SETMASK) g_sig_mask = m;
            else { set_result(c, 0, EINVAL); return; }
            g_sig_mask &= ~SIG_NEVER;
            if (g_sig_mask != old) sig_sync();
        }
        if (prev) st32(prev, old);
        set_result(c, 0, 0);              /* pending signals that became unblocked are delivered by the dispatcher right afterwards */
        return;
    }
    case 111:                               /* sigsuspend(mask): wait until a signal is pending for the guest */
        g_sig_after = g_sig_mask;
        g_sig_mask = arg(0) & ~SIG_NEVER;
        sig_sync();
        sig_wait_for(~g_sig_mask);
        g_sig_after_valid = 1;
        set_result(c, 0, EINTR);
        return;
    case 52:                                /* sigpending(set): pending and blocked */
        if (arg(0)) st32(arg(0), g_sig_pending & g_sig_mask);
        set_result(c, 0, 0);
        return;
    case 330: {                             /* __sigwait(set, sig): take one of the (blocked) signals in set */
        uint32_t want = ld32(arg(0)) & ~SIG_NEVER, got;
        int sig;
        if (!want) { set_result(c, 0, EINVAL); return; }
        sig_wait_for(want | ~g_sig_mask);
        got = g_sig_pending & want;
        if (!got) { set_result(c, 0, EINTR); return; }  /* an unblocked signal came first: its handler runs */
        for (sig = 1; !(got & SIG_BIT(sig)); sig++) ;
        __sync_fetch_and_and(&g_sig_pending, ~SIG_BIT(sig));
        if (arg(1)) st32(arg(1), (uint32_t)sig);
        set_result(c, 0, 0);
        return;
    }
    case 184:                               /* sigreturn(uctx, sigstyle) */
        if (arg(1) == 0x40000000u || arg(1) == 0x80000000u) { set_result(c, 0, 0); return; }    /* alternate stack: none */
        if (!arg(0)) { set_result(c, 0, EINVAL); return; }
        sig_back(c, arg(0));             /* EJUSTRETURN: no result in eax */
        return;
    case 83: case 86: {                     /* setitimer(which, new, old), getitimer(which, old): host clock */
        struct itimerval fresh, prev;
        uint32_t pn = n == 83 ? arg(1) : 0, pa = n == 83 ? arg(2) : arg(1);
        int r;
        if (pn) {
            fresh.it_interval.tv_sec = (time_t)ld32(pn); fresh.it_interval.tv_usec = (suseconds_t)ld32(pn + 4);
            fresh.it_value.tv_sec = (time_t)ld32(pn + 8); fresh.it_value.tv_usec = (suseconds_t)ld32(pn + 12);
        }
        r = n == 83 ? setitimer((int)arg(0), pn ? &fresh : 0, &prev) : getitimer((int)arg(0), &prev);
        if (r < 0) { set_result(c, 0, errno); return; }
        if (pa) {
            st32(pa, (uint32_t)prev.it_interval.tv_sec); st32(pa + 4, (uint32_t)prev.it_interval.tv_usec);
            st32(pa + 8, (uint32_t)prev.it_value.tv_sec); st32(pa + 12, (uint32_t)prev.it_value.tv_usec);
        }
        set_result(c, 0, 0);
        return;
    }
    case 334:                               /* __semwait_signal(cond, mutex, timeout, relative, sec, nsec), also the
                                               _nocancel variant (423): usleep, nanosleep, pthread_cond_timedwait;
                                               only numbers and port names, interruptible (EINTR for the handler) */
        HOST(f_syscall(334, (int)arg(0), (int)arg(1), (int)arg(2), (int)arg(3), (long)arg(4), (int)arg(5)));
        return;
    case 332:                               /* __pthread_markcancel(thread port): guest threads are host threads, so */
        HOST(syscall(332, arg(0)));          /* the kernel marks the right one and interrupts its waits */
        return;
    case 333:                               /* __pthread_canceled(action): enable/disable/test for the calling thread */
        HOST(syscall(333, (int)arg(0)));
        return;
    case 328:                               /* __pthread_kill(thread, sig): only one thread */
        if (arg(1)) sig_schedule((int)arg(1), 0);
        set_result(c, 0, 0);
        return;
    case SYS_sigaltstack:
        if (arg(1)) { st32(arg(1), 0); st32(arg(1) + 4, 0); st32(arg(1) + 8, SS_DISABLE); }
        set_result(c, 0, 0);
        return;
    case SYS_kill:
        /* Signals the translator uses itself (SIGSEGV, SIGBUS, SIGILL, ...) sent to itself are only marked pending.
           All others go through the host: its disposition matches the guest's (handler or blocked: caught and
           marked pending, delivered by the dispatcher right afterwards; ignored; default action), and kqueue
           (EVFILT_SIGNAL) sees them. A group (0, < 0) also hits us via the host handler. */
        if ((pid_t)arg(0) == getpid() && arg(1) && arg(1) < 32 && sig_own((int)arg(1)) && arg(1) != SIGKILL &&
            arg(1) != SIGSTOP) {
            sig_schedule((int)arg(1), 0x10001 /* SI_USER */);
            set_result(c, 0, 0);
            return;
        }
        HOST(kill((pid_t)arg(0), (int)arg(1)));
        return;
    case 294:                               /* shared_region_check_np: no shared i386 region */
        set_result(c, 0, EINVAL);
        return;
    case 366:                               /* bsdthread_register(_thread_start, _wqthread, sizeof pthread) */
        set_result(c, 0, thread_register(arg(0), arg(1), arg(2)));
        return;
    case 360: {                             /* bsdthread_create(fn, arg, stack or size, pthread, flags) */
        uint32_t w = 0;
        int f = thread_spawn(c, arg(0), arg(1), arg(2), arg(3), arg(4), &w);
        set_result(c, w, f);
        return;
    }
    case 367:                               /* workq_open: work queues (thread.c) */
        set_result(c, 0, thread_wq_open());
        return;
    case 368:                               /* workq_ops(op, item, prio) */
        set_result(c, 0, thread_wq_ops(c, arg(0), arg(1), arg(2)));
        return;
    case 142: {                             /* gethostuuid(uuid[16], timespec *timeout): CFPreferences per machine */
        struct timespec ts;
        if (arg(1)) { ts.tv_sec = (time_t)ld32(arg(1)); ts.tv_nsec = (long)ld32(arg(1) + 4); }
        HOST(syscall(142, PTR_ARG(0), arg(1) ? &ts : 0));
        return;
    }
    case 331:                               /* __disable_threadsignal (pthread_exit): signals go to the process */
        set_result(c, 0, 0);
        return;
    case 361:                               /* bsdthread_terminate(stack, size, port, sem): only this thread */
        thread_finish(c, arg(0), arg(1), arg(2), arg(3));
        return;
    case SYS_wait4: {
        int status = 0;
        pid_t r = f_wait4((pid_t)arg(0), arg(1) ? &status : 0, (int)arg(2));
        if (r < 0) { set_result(c, 0, errno); return; }
        if (arg(1)) st32(arg(1), (uint32_t)status);
        set_result(c, (uint32_t)r, 0);
        return;
    }
    case SYS_getlogin: HOST(syscall(SYS_getlogin, PTR_ARG(0), arg(1))); return;
    case SYS_fchmod: HOST(fchmod((int)arg(0), (mode_t)arg(1))); return;
    case SYS_chmod: HOST(chmod((const char *)PTR_ARG(0), (mode_t)arg(1))); return;
    case SYS_ftruncate: HOST(ftruncate((int)arg(0), (off_t)arg64(1))); return;
    case SYS_symlink: HOST(symlink((const char *)PTR_ARG(0), (const char *)PTR_ARG(1))); return;
    case SYS_link: HOST(link((const char *)PTR_ARG(0), (const char *)PTR_ARG(1))); return;
    case SYS_flock: HOST(flock((int)arg(0), (int)arg(1))); return;
    case SYS_utimes: {
        struct timeval t[2];
        if (!arg(1)) { HOST(utimes((const char *)PTR_ARG(0), 0)); return; }
        ts_read(arg(1), &t[0]); ts_read(arg(1) + 8, &t[1]);
        HOST(utimes((const char *)PTR_ARG(0), t));
        return;
    }
    case SYS_getgroups: {
        gid_t g[64];
        int r = getgroups(arg(0) < 64 ? (int)arg(0) : 64, g), i;
        if (r < 0) { set_result(c, 0, errno); return; }
        for (i = 0; i < r && arg(0); i++) st32(arg(1) + 4 * (uint32_t)i, g[i]);
        set_result(c, (uint32_t)r, 0);
        return;
    }
    /* ---------- processes ---------- */
    case SYS_fork: case 66: {               /* fork, vfork */
        pid_t p = proc_fork(c);
        if (p < 0) { set_result(c, 0, errno); return; }
        c->r[EDX] = p == 0;
        set_result(c, p == 0 ? (uint32_t)getpid() : (uint32_t)p, 0);
        return;
    }
    case 59:                                /* execve(path, argv, envp): returns only on failure */
        set_result(c, 0, proc_exec((const char *)PTR_ARG(0), arg(1), arg(2), g_sig_mask));
        return;
    case 244:                               /* posix_spawn(pid *, path, desc *, argv, envp) */
        set_result(c, 0, proc_spawn(arg(0), (const char *)PTR_ARG(1), arg(2), arg(3), arg(4)));
        return;
    case SYS_waitid: {                      /* waitid(idtype, id, siginfo *, options): siginfo 64 bytes */
        siginfo_t si;
        long r;
        memset(&si, 0, sizeof si);
        r = f_syscall(SYS_waitid, (int)arg(0), (int)arg(1), (int)(uintptr_t)&si, (int)arg(3), 0, 0);
        if (r < 0) { set_result(c, 0, errno); return; }
        if (arg(2)) {
            uint32_t a = arg(2);
            memset((void *)(uintptr_t)a, 0, 64);
            st32(a, (uint32_t)si.si_signo); st32(a + 4, (uint32_t)si.si_errno); st32(a + 8, (uint32_t)si.si_code);
            st32(a + 12, (uint32_t)si.si_pid); st32(a + 16, (uint32_t)si.si_uid); st32(a + 20, (uint32_t)si.si_status);
            st32(a + 24, (uint32_t)(uintptr_t)si.si_addr); st32(a + 28, (uint32_t)si.si_value.sival_int);
            st32(a + 32, (uint32_t)si.si_band);
        }
        set_result(c, 0, 0);
        return;
    }
    case SYS_setsid: HOST(setsid()); return;
    case SYS_setpgid: HOST(setpgid((pid_t)arg(0), (pid_t)arg(1))); return;
    case SYS_getpgid: HOST(getpgid((pid_t)arg(0))); return;
    case 310: HOST(getsid((pid_t)arg(0))); return;
    case SYS_setuid: HOST(setuid((uid_t)arg(0))); return;
    case SYS_setgid: HOST(setgid((gid_t)arg(0))); return;
    case SYS_seteuid: HOST(seteuid((uid_t)arg(0))); return;
    case SYS_setegid: HOST(setegid((gid_t)arg(0))); return;
    case SYS_setreuid: HOST(setreuid((uid_t)arg(0), (uid_t)arg(1))); return;
    case SYS_setregid: HOST(setregid((gid_t)arg(0), (gid_t)arg(1))); return;
    case SYS_setgroups: {
        gid_t g[64];
        uint32_t k, cnt = arg(0);
        if (cnt > 64) { set_result(c, 0, EINVAL); return; }
        for (k = 0; k < cnt; k++) g[k] = ld32(arg(1) + 4 * k);
        HOST(setgroups((int)cnt, g));
        return;
    }
    case SYS_setlogin: HOST(setlogin((const char *)PTR_ARG(0))); return;
    case 286: {                             /* gettid(uid *, gid *): identity of this thread */
        uid_t u;
        gid_t g;
        if (syscall(286, &u, &g) < 0) { set_result(c, 0, errno); return; }
        st32(arg(0), u); st32(arg(1), g);
        set_result(c, 0, 0);
        return;
    }
    case 285: HOST(syscall(285, arg(0), arg(1))); return;      /* settid(uid, gid) */
    case SYS_getpriority: {                 /* -1 is a valid priority */
        int r;
        errno = 0;
        r = getpriority((int)arg(0), (id_t)arg(1));
        if (r == -1 && errno) set_result(c, 0, errno); else set_result(c, (uint32_t)r, 0);
        return;
    }
    case SYS_setpriority: HOST(setpriority((int)arg(0), (id_t)arg(1), (int)arg(2))); return;
    case 322: {                             /* iopolicysys(cmd, {scope, iotype, policy}) */
        int p[3], k;
        uint32_t a = arg(1);
        for (k = 0; k < 3; k++) p[k] = (int)ld32(a + 4 * (uint32_t)k);
        if (syscall(322, (int)arg(0), p) < 0) { set_result(c, 0, errno); return; }
        for (k = 0; k < 3; k++) st32(a + 4 * (uint32_t)k, (uint32_t)p[k]);
        set_result(c, 0, 0);
        return;
    }
    case SYS_ptrace:                        /* PT_DENY_ATTACH (anti-debugging): accepted; tracing: not possible */
        set_result(c, 0, arg(0) == 31 ? 0 : EPERM);
        return;
    case 169:                               /* csops(pid, ops, addr, size): status words in guest byte order */
        if (arg(3) == 4) {
            uint32_t v = 0;
            if (syscall(169, (int)arg(0), (int)arg(1), &v, 4) < 0) { set_result(c, 0, errno); return; }
            st32(arg(2), v);
            set_result(c, 0, 0);
            return;
        }
        HOST(syscall(169, (int)arg(0), (int)arg(1), PTR_ARG(2), arg(3)));
        return;
    case 180: set_result(c, 0, 0); return;  /* kdebug_trace */
    case 380:                               /* __mac_execve(path, argv, envp, label): the label is not passed on */
        set_result(c, 0, proc_exec((const char *)PTR_ARG(0), arg(1), arg(2), g_sig_mask));
        return;
    case 381 ... 395: case 424 ... 426:     /* MAC framework (sandbox, quarantine, labels): not offered to the guest */
        set_result(c, 0, ENOSYS);
        return;
    case 165:                               /* quotactl: as on a volume without quotas */
        set_result(c, 0, ENOTSUP);
        return;
    case 167: case 159:                     /* mount, unmount: only root may (file system arguments are not converted) */
        set_result(c, 0, geteuid() ? EPERM : ENOTSUP);
        return;
    case 44: set_result(c, 0, 0); return;   /* profil: no kernel profiling of translated code */
    /* ---------- files ---------- */
    case SYS_chown: HOST(chown((const char *)PTR_ARG(0), (uid_t)arg(1), (gid_t)arg(2))); return;
    case SYS_fchown: HOST(fchown((int)arg(0), (uid_t)arg(1), (gid_t)arg(2))); return;
    case 364: HOST(lchown((const char *)PTR_ARG(0), (uid_t)arg(1), (gid_t)arg(2))); return;
    case SYS_chflags: HOST(chflags((const char *)PTR_ARG(0), arg(1))); return;
    case SYS_fchflags: HOST(fchflags((int)arg(0), arg(1))); return;
    case SYS_truncate: HOST(truncate((const char *)PTR_ARG(0), (off_t)arg64(1))); return;
    case SYS_mknod: HOST(mknod((const char *)PTR_ARG(0), (mode_t)arg(1), (dev_t)arg(2))); return;
    case SYS_mkfifo: HOST(mkfifo((const char *)PTR_ARG(0), (mode_t)arg(1))); return;
    case SYS_revoke: HOST(revoke((const char *)PTR_ARG(0))); return;
    case SYS_chroot: HOST(chroot((const char *)PTR_ARG(0))); return;
    case SYS_sync: sync(); set_result(c, 0, 0); return;
    case 223: HOST(syscall(223, PTR_ARG(0), PTR_ARG(1), arg(2))); return;      /* exchangedata(path, path, options) */
    case 227: HOST(syscall(227, PTR_ARG(0), PTR_ARG(1), arg(2), arg(3))); return;   /* copyfile(from, to, mode, flags) */
    case SYS_futimes: {
        struct timeval t[2];
        if (!arg(1)) { HOST(futimes((int)arg(0), 0)); return; }
        ts_read(arg(1), &t[0]); ts_read(arg(1) + 8, &t[1]);
        HOST(futimes((int)arg(0), t));
        return;
    }
    case 221: {                             /* setattrlist(path, list, buffer, size, options) */
        struct attrlist l;
        uint32_t size = arg(3);
        uint8_t *b = malloc(size ? size : 4);
        int r, e;
        if (!b) { set_result(c, 0, ENOMEM); return; }
        attrlist_read(arg(1), &l);
        memcpy(b, PTR_ARG(2), size);
        attr_swap_fields((uint32_t)(uintptr_t)b, 0, size, &l, "setattrlist");
        r = setattrlist((const char *)PTR_ARG(0), &l, b, size, (unsigned)arg(4));
        e = errno;
        free(b);
        set_result(c, 0, r < 0 ? e : 0);
        return;
    }
    case 348: HOST(syscall(348, PTR_ARG(0))); return;          /* __pthread_chdir: per thread, guest threads are host threads */
    case 349: HOST(syscall(349, (int)arg(0))); return;         /* __pthread_fchdir */
    case 337: {                             /* sendfile(fd, socket, offset (64), off_t *len, sf_hdtr *, flags) */
        off_t len = arg(4) ? (off_t)ld64(arg(4)) : 0;
        struct sf_hdtr h;
        struct iovec hv[16], tv[16];
        uint32_t ha = arg(5), k;
        int r, e;
        if (ha) {
            h.hdr_cnt = (int)ld32(ha + 4); h.trl_cnt = (int)ld32(ha + 12);
            if ((uint32_t)h.hdr_cnt > 16 || (uint32_t)h.trl_cnt > 16) { set_result(c, 0, EINVAL); return; }
            for (k = 0; k < (uint32_t)h.hdr_cnt; k++) {
                hv[k].iov_base = (void *)(uintptr_t)ld32(ld32(ha) + 8 * k); hv[k].iov_len = ld32(ld32(ha) + 8 * k + 4);
            }
            for (k = 0; k < (uint32_t)h.trl_cnt; k++) {
                tv[k].iov_base = (void *)(uintptr_t)ld32(ld32(ha + 8) + 8 * k); tv[k].iov_len = ld32(ld32(ha + 8) + 8 * k + 4);
            }
            h.headers = h.hdr_cnt ? hv : 0;
            h.trailers = h.trl_cnt ? tv : 0;
        }
        r = f_sendfile((int)arg(0), (int)arg(1), (off_t)arg64(2), &len, ha ? &h : 0, (int)arg(6));
        e = errno;
        if (arg(4)) st64(arg(4), (uint64_t)len);    /* also after a partial transfer (EAGAIN, EINTR) */
        set_result(c, 0, r < 0 ? e : 0);
        return;
    }
    /* *_extended: like the plain calls; ACLs (filesec) are not passed on */
    case 277: {                             /* open_extended(path, flags, uid, gid, mode, filesec) */
        int r = open(gpath_open(arg(0), (int)arg(1)), (int)arg(1), (int)arg(4));
        if (r >= 0) jit_fd_off(r);
        HOST(r);
        return;
    }
    case 278: set_result(c, umask((mode_t)arg(0)), 0); return;                /* umask_extended(mask, filesec) */
    case 279: case 280: case 281: {         /* stat_extended, lstat_extended, fstat_extended(x, stat *, filesec, size *) */
        struct stat s;
        int r = n == 279 ? stat(gpath(arg(0)), &s) : n == 280 ? lstat(gpath(arg(0)), &s) : fstat((int)arg(0), &s);
        if (r < 0) { set_result(c, 0, errno); return; }
        stat_write(arg(1), &s);
        if (arg(3)) st32(arg(3), 0);        /* no ACL reported */
        set_result(c, 0, 0);
        return;
    }
    case 282: case 283: {                   /* chmod_extended(path, uid, gid, mode, filesec), fchmod_extended(fd, ...) */
        uint32_t uid = arg(1), gid = arg(2), mode = arg(3);
        int r = 0;
        if (mode != 0xffffffffu) r = n == 282 ? chmod((const char *)PTR_ARG(0), (mode_t)mode) : fchmod((int)arg(0), (mode_t)mode);
        if (r == 0 && (uid != KAUTH_ID_NONE || gid != KAUTH_ID_NONE)) {
            uid_t u = uid == KAUTH_ID_NONE ? (uid_t)-1 : uid;
            gid_t g = gid == KAUTH_ID_NONE ? (gid_t)-1 : gid;
            r = n == 282 ? chown((const char *)PTR_ARG(0), u, g) : fchown((int)arg(0), u, g);
        }
        HOST(r);
        return;
    }
    case 291: HOST(mkfifo((const char *)PTR_ARG(0), (mode_t)arg(3))); return;    /* mkfifo_extended */
    case 292: HOST(mkdir((const char *)PTR_ARG(0), (mode_t)arg(3))); return;     /* mkdir_extended */
    /* ---------- memory ---------- */
    case SYS_mlock: HOST(mlock(PTR_ARG(0), arg(1))); return;
    case SYS_munlock: HOST(munlock(PTR_ARG(0), arg(1))); return;
    case 324: HOST(mlockall((int)arg(0))); return;
    case 325: HOST(munlockall()); return;
    case SYS_minherit: HOST(minherit(PTR_ARG(0), arg(1), (int)arg(2))); return;
    case SYS_mincore: HOST(mincore(PTR_ARG(0), arg(1), PTR_ARG(2))); return;
    /* ---------- POSIX semaphores and shared memory: handles are plain numbers ---------- */
    case 267: HOST(syscall(267, PTR_ARG(0))); return;          /* shm_unlink(name) */
    case 268: HOST(syscall(268, PTR_ARG(0), (int)arg(1), (int)arg(2), (int)arg(3))); return;    /* sem_open */
    case 269: HOST(syscall(269, (int)arg(0))); return;         /* sem_close */
    case 270: HOST(syscall(270, PTR_ARG(0))); return;          /* sem_unlink */
    case 271: HOST(f_syscall(271, (int)arg(0), 0, 0, 0, 0, 0)); return;   /* sem_wait (also _nocancel) */
    case 272: HOST(syscall(272, (int)arg(0))); return;         /* sem_trywait */
    case 273: HOST(syscall(273, (int)arg(0))); return;         /* sem_post */
    case 274: case 275: case 276: set_result(c, 0, ENOSYS); return;   /* sem_getvalue, sem_init, sem_destroy: none in xnu */
    /* ---------- kqueue ---------- */
    case 362: HOST(kqueue()); return;
    case 363: {                             /* kevent(kq, changes, n, events, n, timeout); struct kevent: ident @0,
                                               filter (16) @4, flags (16) @6, fflags @8, data @12, udata @16 */
        int nch = (int)arg(2), nev = (int)arg(4), r, e, k;
        uint32_t ga = arg(1), gb = arg(3);
        struct kevent chs[16], evs[16], *ch = chs, *ev = evs;
        struct timespec ts;
        if (nch < 0 || nev < 0 || nch > 65536 || nev > 65536) { set_result(c, 0, EINVAL); return; }
        if (nch > 16 && !(ch = malloc((size_t)nch * sizeof *ch))) { set_result(c, 0, ENOMEM); return; }
        if (nev > 16 && !(ev = malloc((size_t)nev * sizeof *ev))) { if (ch != chs) free(ch); set_result(c, 0, ENOMEM); return; }
        for (k = 0; k < nch; k++) {
            uint32_t a = ga + 20 * (uint32_t)k;
            ch[k].ident = ld32(a); ch[k].filter = (int16_t)ld16(a + 4); ch[k].flags = ld16(a + 6);
            ch[k].fflags = ld32(a + 8); ch[k].data = (intptr_t)(int32_t)ld32(a + 12); ch[k].udata = (void *)(uintptr_t)ld32(a + 16);
        }
        if (arg(5)) { ts.tv_sec = (time_t)(int32_t)ld32(arg(5)); ts.tv_nsec = (long)(int32_t)ld32(arg(5) + 4); }
        if (g_log) {
            for (k = 0; k < nch; k++)
                fprintf(stderr, "[kevent] change ident %u filter %d flags %04x fflags %x data %ld udata %08x\n", (unsigned)ch[k].ident,
                        ch[k].filter, ch[k].flags, ch[k].fflags, (long)ch[k].data, (unsigned)(uintptr_t)ch[k].udata);
            if (arg(5)) fprintf(stderr, "[kevent] wait up to %ld.%09ld s for %d events\n", (long)ts.tv_sec, ts.tv_nsec, nev);
        }
        r = f_kevent((int)arg(0), ch, nch, ev, nev, arg(5) ? &ts : 0);
        e = errno;
        if (g_log)
            for (k = 0; k < r; k++)
                fprintf(stderr, "[kevent] event ident %u filter %d flags %04x fflags %x data %ld udata %08x\n", (unsigned)ev[k].ident,
                        ev[k].filter, ev[k].flags, ev[k].fflags, (long)ev[k].data, (unsigned)(uintptr_t)ev[k].udata);
        for (k = 0; k < r; k++) {
            uint32_t a = gb + 20 * (uint32_t)k;
            st32(a, (uint32_t)ev[k].ident); st16(a + 4, (uint16_t)ev[k].filter); st16(a + 6, ev[k].flags);
            st32(a + 8, ev[k].fflags); st32(a + 12, (uint32_t)ev[k].data); st32(a + 16, (uint32_t)(uintptr_t)ev[k].udata);
        }
        if (ch != chs) free(ch);
        if (ev != evs) free(ev);
        set_result(c, (uint32_t)(r < 0 ? 0 : r), r < 0 ? e : 0);
        return;
    }
    default: {
        static uint8_t reported[1024];      /* report each number only once (retry loops flooded the output) */
        if (n >= 1024 || !reported[n]++)
            fprintf(stderr, "rosetta: BSD call %u not implemented yet (eip %08x, arguments %08x %08x %08x)\n", n, c->eip,
                    arg(0), arg(1), arg(2));
        set_result(c, 0, ENOSYS);
        return;
    }
    }
}

/* ---------- machine-dependent (int $0x82) ---------- */

void sys_machine(Cpu *c)
{
    uint32_t n = c->r[EAX];
    g_arg_base = c->r[ESP];
    if (g_log) fprintf(stderr, "[machine %u] %08x\n", n, arg(0));
    switch (n) {
    case 3:                                 /* thread_fast_set_cthread_self: base of gs */
        c->gs_base = arg(0);
        c->r[EAX] = 0x37;                   /* selector as under Intel 10.5 */
        return;
    default:
        fprintf(stderr, "rosetta: machine-dependent call %u not implemented yet (eip %08x)\n", n, c->eip);
        c->r[EAX] = (uint32_t)-1;
    }
}

void sys_mach(Cpu *c)
{
    g_arg_base = c->r[ESP];
    mach_trap(c, (int)-(int32_t)c->r[EAX], g_arg_base);
}

/* time per system call (ROSETTA_STATS): index is the BSD number, Mach traps from 512 */
static uint64_t g_sys_time[1024];
static unsigned g_sys_n[1024];
static int g_sys_measure = -1;

void sys_stats(void)
{
    mach_timebase_info_data_t tb;
    int k, best, z;
    uint64_t sum = 0;
    if (g_sys_measure <= 0) return;
    if (g_intel_on > 0) fprintf(stderr, "rosetta: %lu paths redirected to Intel versions (%s)\n", g_st_redirected, g_intel);
    fprintf(stderr, "rosetta: %lu file mappings shared, %lu made private later\n", g_st_shared, g_st_private);
    mach_timebase_info(&tb);
    for (k = 0; k < 1024; k++) sum += g_sys_time[k];
    fprintf(stderr, "rosetta: system calls %.2f ms, most expensive:", (double)sum * tb.numer / tb.denom / 1e6);
    for (z = 0; z < 12; z++) {
        for (best = -1, k = 0; k < 1024; k++) if (g_sys_n[k] && (best < 0 || g_sys_time[k] > g_sys_time[best])) best = k;
        if (best < 0) break;
        fprintf(stderr, " %s%d:%ux/%.2f", best >= 512 ? "mach" : "", best >= 512 ? best - 512 : best, g_sys_n[best],
                (double)g_sys_time[best] * tb.numer / tb.denom / 1e6);
        g_sys_n[best] = 0;
    }
    fprintf(stderr, " ms\n");
}

void sys_sysenter(Cpu *c)
{
    uint32_t back = c->r[EDX], stack = c->r[ECX];
    int nr = (int32_t)c->r[EAX] < 0 ? 512 + (int)((-(int32_t)c->r[EAX]) & 511) : (int)(c->r[EAX] & 511);
    uint64_t t0 = 0;
    if (g_sys_measure < 0) g_sys_measure = getenv("ROSETTA_STATS") != 0;
    if (g_sys_measure) t0 = mach_absolute_time();
    g_arg_base = stack;
    g_sig_eip_set = 0;
    if ((int32_t)c->r[EAX] < 0) mach_trap(c, (int)-(int32_t)c->r[EAX], stack);
    else sys_bsd(c, 1);
    if (g_sys_measure) { g_sys_time[nr] += mach_absolute_time() - t0; g_sys_n[nr]++; }
    if (g_sig_eip_set) { g_sig_eip_set = 0; return; }     /* sigreturn: state is already set */
    c->eip = back;
    c->r[ESP] = stack;
}
