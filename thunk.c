/* thunk.c - replace functions of the Intel libSystem with native ones.
 *
 * Math functions (sin, pow, ...) and string functions (strlen, strcmp, ...) of the i386 libSystem are not translated
 * instruction by instruction: at their entry address the translator places a stub that fetches the arguments from
 * the Intel stack, calls the PowerPC version and stores the result according to the i386 calling convention
 * (double/float in st(0), integers and pointers in eax). The stub block does the return itself (jit.c).
 *
 * The addresses come from the symbol table of the loaded libSystem: vm_region finds an image (Mach header at the
 * start of its __TEXT range), __LINKEDIT is in memory. Names with a suffix ($fenv_access_off, $UNIX2003) count as
 * the base name.
 */
#include "thunk.h"
#include "gl.h"
#include "al.h"
#include "ca.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>

/* ---------- arguments and result ---------- */

static uint32_t arg32(Cpu *c, int k) { return ld32(c->r[ESP] + 4 + 4 * (uint32_t)k); }
static double arg_d(Cpu *c, int k) { union { uint64_t u; double d; } v; v.u = ld64(c->r[ESP] + 4 + 4 * (uint32_t)k); return v.d; }
static float arg_f(Cpu *c, int k) { union { uint32_t u; float f; } v; v.u = arg32(c, k); return v.f; }
#define PTR_ARG(k) ((char *)(uintptr_t)arg32(c, k))

/* result in st(0): push as in the interpreter */
static void st0(Cpu *c, double v)
{
    c->st_top = (c->st_top - 1) & 7;
    c->st[c->st_top] = v;
    if (c->x87_depth < 8) c->x87_depth++;
}

/* ---------- stubs ---------- */

#define D_D(n)  static int t_##n(Cpu *c) { st0(c, n(arg_d(c, 0))); return 0; }
#define D_DD(n) static int t_##n(Cpu *c) { st0(c, n(arg_d(c, 0), arg_d(c, 2))); return 0; }
#define F_F(n)  static int t_##n(Cpu *c) { st0(c, (double)n(arg_f(c, 0))); return 0; }
#define F_FF(n) static int t_##n(Cpu *c) { st0(c, (double)n(arg_f(c, 0), arg_f(c, 1))); return 0; }

D_D(sin) D_D(cos) D_D(tan) D_D(asin) D_D(acos) D_D(atan) D_D(sinh) D_D(cosh) D_D(tanh)
D_D(asinh) D_D(acosh) D_D(atanh) D_D(exp) D_D(exp2) D_D(expm1) D_D(log) D_D(log10) D_D(log2) D_D(log1p)
D_D(sqrt) D_D(cbrt) D_D(floor) D_D(ceil) D_D(trunc) D_D(round) D_D(rint) D_D(nearbyint) D_D(fabs) D_D(erf) D_D(erfc)
D_DD(pow) D_DD(atan2) D_DD(fmod) D_DD(hypot) D_DD(remainder) D_DD(copysign) D_DD(fmin) D_DD(fmax) D_DD(fdim)
F_F(sinf) F_F(cosf) F_F(tanf) F_F(asinf) F_F(acosf) F_F(atanf) F_F(expf) F_F(exp2f) F_F(logf) F_F(log10f)
F_F(log2f) F_F(sqrtf) F_F(floorf) F_F(ceilf) F_F(truncf) F_F(roundf) F_F(fabsf)
F_FF(powf) F_FF(atan2f) F_FF(fmodf) F_FF(hypotf)

static int t_ldexp(Cpu *c) { st0(c, ldexp(arg_d(c, 0), (int)arg32(c, 2))); return 0; }
static int t_ldexpf(Cpu *c) { st0(c, (double)ldexpf(arg_f(c, 0), (int)arg32(c, 1))); return 0; }

static int t_strlen(Cpu *c) { c->r[EAX] = (uint32_t)strlen(PTR_ARG(0)); return 0; }
static int t_strcmp(Cpu *c) { c->r[EAX] = (uint32_t)strcmp(PTR_ARG(0), PTR_ARG(1)); return 0; }
static int t_strncmp(Cpu *c) { c->r[EAX] = (uint32_t)strncmp(PTR_ARG(0), PTR_ARG(1), arg32(c, 2)); return 0; }
static int t_memcmp(Cpu *c) { c->r[EAX] = (uint32_t)memcmp(PTR_ARG(0), PTR_ARG(1), arg32(c, 2)); return 0; }
static int t_strcpy(Cpu *c) { strcpy(PTR_ARG(0), PTR_ARG(1)); c->r[EAX] = arg32(c, 0); return 0; }
static int t_strncpy(Cpu *c) { strncpy(PTR_ARG(0), PTR_ARG(1), arg32(c, 2)); c->r[EAX] = arg32(c, 0); return 0; }
static int t_strcat(Cpu *c) { strcat(PTR_ARG(0), PTR_ARG(1)); c->r[EAX] = arg32(c, 0); return 0; }
static int t_strchr(Cpu *c) { c->r[EAX] = (uint32_t)(uintptr_t)strchr(PTR_ARG(0), (int)arg32(c, 1)); return 0; }
static int t_strrchr(Cpu *c) { c->r[EAX] = (uint32_t)(uintptr_t)strrchr(PTR_ARG(0), (int)arg32(c, 1)); return 0; }
static int t_memchr(Cpu *c) { c->r[EAX] = (uint32_t)(uintptr_t)memchr(PTR_ARG(0), (int)arg32(c, 1), arg32(c, 2)); return 0; }
static int t_strstr(Cpu *c) { c->r[EAX] = (uint32_t)(uintptr_t)strstr(PTR_ARG(0), PTR_ARG(1)); return 0; }
/* strtok keeps its pointer in the host libc: this works because every guest call comes here */
static int t_strtok(Cpu *c) { c->r[EAX] = (uint32_t)(uintptr_t)strtok(PTR_ARG(0), PTR_ARG(1)); return 0; }
static int t_strtok_r(Cpu *c)
{
    char *z = (char *)(uintptr_t)ld32(arg32(c, 2)), *e;     /* char **lasts: pointer in guest memory (Intel byte order) */
    e = strtok_r(PTR_ARG(0), PTR_ARG(1), &z);
    st32(arg32(c, 2), (uint32_t)(uintptr_t)z);
    c->r[EAX] = (uint32_t)(uintptr_t)e;
    return 0;
}
static int t_strspn(Cpu *c) { c->r[EAX] = (uint32_t)strspn(PTR_ARG(0), PTR_ARG(1)); return 0; }
static int t_strcspn(Cpu *c) { c->r[EAX] = (uint32_t)strcspn(PTR_ARG(0), PTR_ARG(1)); return 0; }
static int t_strpbrk(Cpu *c) { c->r[EAX] = (uint32_t)(uintptr_t)strpbrk(PTR_ARG(0), PTR_ARG(1)); return 0; }
static int t_strncat(Cpu *c) { strncat(PTR_ARG(0), PTR_ARG(1), arg32(c, 2)); c->r[EAX] = arg32(c, 0); return 0; }
static int t_strcasecmp(Cpu *c) { c->r[EAX] = (uint32_t)strcasecmp(PTR_ARG(0), PTR_ARG(1)); return 0; }
static int t_strncasecmp(Cpu *c) { c->r[EAX] = (uint32_t)strncasecmp(PTR_ARG(0), PTR_ARG(1), arg32(c, 2)); return 0; }
static int t_atoi(Cpu *c) { c->r[EAX] = (uint32_t)atoi(PTR_ARG(0)); return 0; }
/* strtol/strtoul: endptr is a char ** in guest memory; the guest errno stays untouched */
static int t_strtol(Cpu *c)
{
    char *end;
    c->r[EAX] = (uint32_t)strtol(PTR_ARG(0), &end, (int)arg32(c, 2));
    if (arg32(c, 1)) st32(arg32(c, 1), (uint32_t)(uintptr_t)end);
    return 0;
}
static int t_strtoul(Cpu *c)
{
    char *end;
    c->r[EAX] = (uint32_t)strtoul(PTR_ARG(0), &end, (int)arg32(c, 2));
    if (arg32(c, 1)) st32(arg32(c, 1), (uint32_t)(uintptr_t)end);
    return 0;
}

/* Calculator's expression engine, CalculatePerformExpression(expression, significant digits, flags, answer), lives in
   the private Calculate.framework, which a PowerPC system has only for PowerPC (the Intel root has a stub library):
   the arguments are strings and numbers, so the PowerPC function serves the guest directly */
static int t_CalculatePerformExpression(Cpu *c)
{
    static int (*fn)(char *, int, int, char *);
    static int tried;
    if (!tried) {
        void *h = dlopen("/System/Library/PrivateFrameworks/Calculate.framework/Versions/A/Calculate", RTLD_LAZY | RTLD_LOCAL);
        tried = 1;
        if (h) fn = (int (*)(char *, int, int, char *))dlsym(h, "CalculatePerformExpression");
        if (!fn) fprintf(stderr, "rosetta: Calculate.framework (PowerPC) not available\n");
    }
    c->r[EAX] = fn ? (uint32_t)fn(PTR_ARG(0), (int)arg32(c, 1), (int)arg32(c, 2), PTR_ARG(3)) : 0;
    return 0;
}

/* art: 'g' only via the C wrapper; '1' double(double), '2' double(double, double), 'f' float(float),
   'F' float(float, float): the translator then calls the PowerPC function itself (arguments in f1/f2, result f1) */
#define S(n)  { #n, t_##n, 'g', 0 }
#define M1(n) { #n, t_##n, '1', (void *)n }
#define M2(n) { #n, t_##n, '2', (void *)n }
#define MF(n) { #n, t_##n, 'f', (void *)n }
#define MG(n) { #n, t_##n, 'F', (void *)n }
static const struct { const char *name; Stub fn; char kind; void *native; } g_list[] = {
    M1(sin), M1(cos), M1(tan), M1(asin), M1(acos), M1(atan), M1(sinh), M1(cosh), M1(tanh), M1(asinh), M1(acosh),
    M1(atanh), M1(exp), M1(exp2), M1(expm1), M1(log), M1(log10), M1(log2), M1(log1p), M1(sqrt), M1(cbrt), M1(floor),
    M1(ceil), M1(trunc), M1(round), M1(rint), M1(nearbyint), M1(fabs), M1(erf), M1(erfc), M2(pow), M2(atan2),
    M2(fmod), M2(hypot), M2(remainder), M2(copysign), M2(fmin), M2(fmax), M2(fdim), MF(sinf), MF(cosf), MF(tanf),
    MF(asinf), MF(acosf), MF(atanf), MF(expf), MF(exp2f), MF(logf), MF(log10f), MF(log2f), MF(sqrtf), MF(floorf),
    MF(ceilf), MF(truncf), MF(roundf), MF(fabsf), MG(powf), MG(atan2f), MG(fmodf), MG(hypotf), S(ldexp), S(ldexpf),
    S(strlen), S(strcmp), S(strncmp), S(memcmp), S(strcpy), S(strncpy), S(strcat), S(strchr), S(strrchr), S(memchr),
    S(strstr), S(strtok), S(strtok_r), S(strspn), S(strcspn), S(strpbrk), S(strncat), S(strcasecmp), S(strncasecmp),
    S(atoi), S(strtol), S(strtoul), S(CalculatePerformExpression),
};

/* frameworks besides libSystem whose exported functions are replaced by name (their i386 version is a stub library
   or does less than the PowerPC one) */
/* OpenGL: gl.c (the Intel OpenGL cannot reach the graphics chip of a PowerPC Mac); OpenAL: al.c (no i386 OpenAL in the
   10.5.8 update: the Intel root has a stub library) */
static const char *const g_native_libs[] = { "/Calculate.framework/", "/OpenGL.framework/", "/OpenAL.framework/",
                                             "/CoreAudio.framework/", 0 };     /* CoreAudio: its HAL (ca.c) */
#define LIST (sizeof g_list / sizeof g_list[0])

/* ---------- tables ---------- */

#define NAMES 256                               /* hash table of the names (index in g_list + 1) */
#define ADDRS 4096                           /* entry address -> stub (libSystem, about 1000 of OpenGL) */
#define IMAGES 128

static unsigned char g_names[NAMES];
static struct { uint32_t addr; Stub fn; } g_addr[ADDRS];
static struct { uint32_t start, end; int libsystem; } g_image[IMAGES];
static int g_nimage, g_names_done, g_off = -1, g_last;

static uint32_t name_scatter(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    while (n--) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

static unsigned char g_first[256];               /* first letters of the names (prefilter when reading the symbols) */
static size_t g_maxlen;

static void names_build(void)
{
    unsigned i;
    for (i = 0; i < LIST; i++) {
        uint32_t h = name_scatter(g_list[i].name, strlen(g_list[i].name)) & (NAMES - 1);
        g_first[(unsigned char)g_list[i].name[0]] = 1;
        if (strlen(g_list[i].name) > g_maxlen) g_maxlen = strlen(g_list[i].name);
        while (g_names[h]) h = (h + 1) & (NAMES - 1);
        g_names[h] = (unsigned char)(i + 1);
    }
    g_names_done = 1;
}

static Stub name_find(const char *s, size_t n)
{
    uint32_t h = name_scatter(s, n) & (NAMES - 1);
    while (g_names[h]) {
        const char *k = g_list[g_names[h] - 1].name;
        if (strlen(k) == n && !memcmp(k, s, n)) return g_list[g_names[h] - 1].fn;
        h = (h + 1) & (NAMES - 1);
    }
    return 0;
}

/* symbol name (without _) up to the $ suffix: stub or 0; long and foreign names drop out quickly */
static Stub name_verify(const char *s)
{
    size_t n = 0;
    if ((s[0] == 'g' && s[1] == 'l') || (s[0] == 'C' && s[1] == 'G')) {     /* OpenGL (gl.c) */
        Stub fn;
        while (s[n] && s[n] != '$') n++;
        if ((fn = gl_stub(s, n)) != 0) return fn;
        n = 0;
    }
    if (s[0] == 'A' && s[1] == 'u' && s[2] == 'd') {      /* CoreAudio's HAL (ca.c) */
        Stub fn;
        while (s[n] && s[n] != '$') n++;
        if ((fn = ca_stub(s, n)) != 0) return fn;
        n = 0;
    }
    if (s[0] == 'a' && s[1] == 'l') {                     /* OpenAL (al.c) */
        Stub fn;
        while (s[n] && s[n] != '$') n++;
        if ((fn = al_stub(s, n)) != 0) return fn;
        n = 0;
    }
    if (!g_first[(unsigned char)s[0]]) return 0;
    while (s[n] && s[n] != '$') if (++n > g_maxlen) return 0;
    return name_find(s, n);
}

static void addr_add(uint32_t a, Stub fn)
{
    uint32_t h = (a * 2654435761u) >> 20, i;
    for (i = 0; i < ADDRS; i++, h = (h + 1) & (ADDRS - 1))
        if (!g_addr[h].fn || g_addr[h].addr == a) { g_addr[h].addr = a; g_addr[h].fn = fn; return; }
}

uint32_t thunk_guest_addr(Stub fn)
{
    uint32_t i;
    for (i = 0; i < ADDRS; i++) if (g_addr[i].fn == fn) return g_addr[i].addr;
    return 0;
}

static Stub addr_find(uint32_t a)
{
    uint32_t h = (a * 2654435761u) >> 20, i;
    for (i = 0; i < ADDRS && g_addr[h].fn; i++, h = (h + 1) & (ADDRS - 1))
        if (g_addr[h].addr == a) return g_addr[h].fn;
    return 0;
}

static void image_note(uint32_t start, uint32_t end, int libsystem)
{
    if (g_nimage == IMAGES) g_nimage = 0;         /* full: start again from the beginning (it is only read again) */
    g_image[g_nimage].start = start;
    g_image[g_nimage].end = end;
    g_image[g_nimage].libsystem = libsystem;
    g_nimage++;
}

/* symbol stubs (jump table entries, jmp *pointer) -> stub via the name from the indirect symbol table: this also
   works before dyld binds the entry */
#define STUBS 4096
static struct { uint32_t addr; Stub fn; } g_stub[STUBS];

static void stub_add(uint32_t a, Stub fn)
{
    uint32_t h = (a * 2654435761u) >> 20, i;
    for (i = 0; i < STUBS; i++, h = (h + 1) & (STUBS - 1))
        if (!g_stub[h].fn || g_stub[h].addr == a) { g_stub[h].addr = a; g_stub[h].fn = fn; return; }
}

/* symbol stub sections of the images read: a symbol stub is resolved only when the translator sees a call to it
   (resolving all of them at load time cost 0.7 ms: per entry indirect symbol, symbol and name at scattered places) */
#define STUB_SECTIONS 64
static struct { uint32_t start, end, step, ind, base, indoff, nind, symoff, nsyms, stroff; } g_sa[STUB_SECTIONS];
static int g_nsa;
static int no_stub(Cpu *c) { (void)c; return 0; }
#define NO_STUB no_stub                 /* cached: this symbol stub leads to no stub */
static unsigned long g_st_stubs_resolved;

static Stub stub_resolve(uint32_t z)
{
    int k;
    for (k = 0; k < g_nsa; k++)
        if (z - g_sa[k].start < g_sa[k].end - g_sa[k].start && (z - g_sa[k].start) % g_sa[k].step == 0) {
            uint32_t idx = g_sa[k].ind + (z - g_sa[k].start) / g_sa[k].step, sym;
            const char *name;
            Stub fn = 0;
            g_st_stubs_resolved++;
            if (idx < g_sa[k].nind) {
                sym = ld32(g_sa[k].base + g_sa[k].indoff + 4 * idx);
                if (!(sym & 0xc0000000u) && sym < g_sa[k].nsyms) {         /* not INDIRECT_SYMBOL_LOCAL/ABS */
                    name = (const char *)(uintptr_t)(g_sa[k].base + g_sa[k].stroff + ld32(g_sa[k].base + g_sa[k].symoff + 12 * sym));
                    if (name[0] == '_') fn = name_verify(name + 1);
                }
            }
            stub_add(z, fn ? fn : NO_STUB);
            return fn;
        }
    return 0;
}

Stub thunk_stub(uint32_t z)
{
    uint32_t h = (z * 2654435761u) >> 20, i;
    if (g_off) return 0;
    for (i = 0; i < STUBS && g_stub[h].fn; i++, h = (h + 1) & (STUBS - 1))
        if (g_stub[h].addr == z) return g_stub[h].fn == NO_STUB ? 0 : g_stub[h].fn;
    return stub_resolve(z);
}

/* read an i386 Mach header in guest memory: remember the range; for libSystem enter the stubs; for every image the
   symbol stubs that lead to stub functions */
static uint64_t g_bl_t0;
static double bl_ms(void)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return (double)(mach_absolute_time() - g_bl_t0) * tb.numer / tb.denom / 1e6;
}

/* store of the stub addresses per libSystem (~/Library/Caches/rosetta/stubs-UUID): saves reading the symbol table
   at startup (0.7 ms, almost only page faults in __LINKEDIT). First line: checksum of the stub list (different
   version of rosetta -> read again), then per line the offset to the Mach header and the name */
#define STORE_MAX 512
static struct { uint32_t offset; Stub fn; } g_new[STORE_MAX];
static int g_new_n;

static uint32_t list_checksum(void)
{
    unsigned k;
    uint32_t h = 2166136261u ^ LIST;
    for (k = 0; k < LIST; k++) h = (h ^ name_scatter(g_list[k].name, strlen(g_list[k].name))) * 16777619u;
    return h;
}

static int store_path(const uint8_t *uuid, char *path, size_t sz, int directory)
{
    const char *home = getenv("HOME");
    int k, o;
    if (!home || !*home) return 0;
    o = snprintf(path, sz, "%s/Library/Caches/rosetta", home);
    if (o <= 0 || (size_t)o + 48 >= sz) return 0;
    if (directory) return 1;
    o += snprintf(path + o, sz - o, "/stubs-");
    for (k = 0; k < 16; k++) o += snprintf(path + o, sz - o, "%02x", uuid[k]);
    return 1;
}

/* file format (host byte order): checksum of the stub list, count, then per entry offset and index in g_list */
static int store_read(const uint8_t *uuid, uint32_t head, uint32_t *n)
{
    char path[1024];
    uint32_t d[2 + 2 * STORE_MAX], k;
    int fd;
    ssize_t got;
    if (getenv("ROSETTA_NO_STORE") || !store_path(uuid, path, sizeof path, 0) || (fd = open(path, O_RDONLY)) < 0) return 0;
    got = read(fd, d, sizeof d);
    close(fd);
    if (got < 8 || d[0] != list_checksum() || d[1] > STORE_MAX || (size_t)got != 8 + 8 * (size_t)d[1]) return 0;
    for (k = 0; k < d[1]; k++) if (d[3 + 2 * k] >= LIST) return 0;
    for (k = 0; k < d[1]; k++) addr_add(head + d[2 + 2 * k], g_list[d[3 + 2 * k]].fn);
    *n = d[1];
    return 1;
}

static void store_write(const uint8_t *uuid)
{
    char path[1024], tmp[1100];
    uint32_t d[2 + 2 * STORE_MAX];
    int k, fd;
    unsigned j;
    if (getenv("ROSETTA_NO_STORE") || !store_path(uuid, path, sizeof path, 1)) return;
    mkdir(path, 0755);
    if (!store_path(uuid, path, sizeof path, 0)) return;
    d[0] = list_checksum();
    d[1] = (uint32_t)g_new_n;
    for (k = 0; k < g_new_n; k++) {
        for (j = 0; j < LIST && g_list[j].fn != g_new[k].fn; j++) ;
        if (j == LIST) return;
        d[2 + 2 * k] = g_new[k].offset;
        d[3 + 2 * k] = j;
    }
    snprintf(tmp, sizeof tmp, "%s.%d", path, (int)getpid());
    if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) return;
    if (write(fd, d, 8 + 8 * (size_t)g_new_n) == (ssize_t)(8 + 8 * (size_t)g_new_n) && close(fd) == 0) rename(tmp, path);
    else { close(fd); unlink(tmp); }
}

static void image_read(uint32_t head, uint32_t region_end)
{
    uint32_t ncmds = ld32(head + 16), lc = head + 28, i;
    uint32_t text_vm = 0, text_sz = 0, le_vm = 0, le_off = 0, symoff = 0, nsyms = 0, stroff = 0, vmslide, base, n = 0;
    uint32_t indoff = 0, nind = 0, st_addr[8], st_sz[8], st_ind[8], st_step[8], iext = 0, next = 0;
    int libsystem = 0, native_lib = 0, text = 0, nst = 0, dyld;
    const uint8_t *uuid = 0;
    for (i = 0; i < ncmds && lc < head + 0x10000; i++) {
        uint32_t cmd = ld32(lc), sz = ld32(lc + 4);
        if (cmd == 1) {                         /* LC_SEGMENT, then the sections (68 bytes each) */
            const char *seg = (const char *)(uintptr_t)(lc + 8);
            uint32_t k, nsect = ld32(lc + 48);
            if (!strncmp(seg, "__TEXT", 16)) { text_vm = ld32(lc + 24); text_sz = ld32(lc + 28); text = 1; }
            else if (!strncmp(seg, "__LINKEDIT", 16)) { le_vm = ld32(lc + 24); le_off = ld32(lc + 32); }
            for (k = 0; k < nsect && k < 64; k++) {
                uint32_t s = lc + 56 + 68 * k;
                if ((ld32(s + 56) & 0xff) == 8 && ld32(s + 64) && nst < 8) {   /* S_SYMBOL_STUBS */
                    st_addr[nst] = ld32(s + 32); st_sz[nst] = ld32(s + 36);
                    st_ind[nst] = ld32(s + 60); st_step[nst] = ld32(s + 64); nst++;
                }
            }
        } else if (cmd == 2) {                  /* LC_SYMTAB */
            symoff = ld32(lc + 8); nsyms = ld32(lc + 12); stroff = ld32(lc + 16);
        } else if (cmd == 0xb) {                /* LC_DYSYMTAB: external symbols, indirect symbols */
            iext = ld32(lc + 16); next = ld32(lc + 20);
            indoff = ld32(lc + 56); nind = ld32(lc + 60);
        } else if (cmd == 0xd) {                /* LC_ID_DYLIB */
            const char *name = (const char *)(uintptr_t)(lc + ld32(lc + 8));
            int k;
            if (strstr(name, "/libSystem.B.dylib")) libsystem = 1;
            for (k = 0; g_native_libs[k]; k++) if (strstr(name, g_native_libs[k])) native_lib = 1;
        } else if (cmd == 0x1b && sz >= 24) {   /* LC_UUID */
            uuid = (const uint8_t *)(uintptr_t)(lc + 8);
        }
        if (sz < 8) break;
        lc += sz;
    }
    if (!text) { image_note(head, region_end, 0); return; }
    vmslide = head - text_vm;
    dyld = ld32(head + 12) == 7;                /* MH_DYLINKER */
    image_note(head, head + text_sz, libsystem || dyld || native_lib);
    if (!symoff || !le_vm || g_off) return;
    base = le_vm + vmslide - le_off;             /* file offset in __LINKEDIT -> address */
    if (dyld) {
        /* dyld has its own (local) copies of the string functions: when binding the frameworks it compares thousands
           of symbol names (strcmp 2 % of the Foundation startup). Same meaning as in libSystem: the same stubs */
        static const char *const dyld_names[] = { "_strcmp", "_strncmp", "_strlen", "_memcmp", "_strcpy", "_strncpy",
                                                  "_strchr", "_strrchr", "_strstr", "_strcat", 0 };
        for (i = 0; i < nsyms; i++) {
            uint32_t s = base + symoff + 12 * i;
            uint8_t type = (uint8_t)ld8(s + 4);
            const char *name;
            int k;
            if ((type & 0xe0) || (type & 0x0e) != 0x0e) continue;     /* N_SECT, local or external */
            name = (const char *)(uintptr_t)(base + stroff + ld32(s));
            for (k = 0; dyld_names[k] && strcmp(name, dyld_names[k]); k++) ;
            if (dyld_names[k]) {
                Stub fn = name_verify(name + 1);
                if (fn) { addr_add(ld32(s + 8) + vmslide, fn); n++; }
            }
        }
        if (getenv("ROSETTA_STATS")) fprintf(stderr, "rosetta: dyld at %08x, %u stubs\n", head, n);
        return;
    }
    if (indoff) {                               /* remember the symbol stub sections (entry j -> indirect symbol -> name, on demand) */
        int k;
        for (k = 0; k < nst; k++) {
            int s = g_nsa < STUB_SECTIONS ? g_nsa++ : (int)(head >> 12) % STUB_SECTIONS;
            g_sa[s].start = st_addr[k] + vmslide;
            g_sa[s].end = st_addr[k] + vmslide + st_sz[k];
            g_sa[s].step = st_step[k];
            g_sa[s].ind = st_ind[k];
            g_sa[s].base = base;
            g_sa[s].indoff = indoff;
            g_sa[s].nind = nind;
            g_sa[s].symoff = symoff;
            g_sa[s].nsyms = nsyms;
            g_sa[s].stroff = stroff;
        }
    }
    if (native_lib && !libsystem) {             /* small symbol table: read every time */
        for (i = next && iext + next <= nsyms ? iext : 0; i < (next && iext + next <= nsyms ? iext + next : nsyms); i++) {
            uint32_t s = base + symoff + 12 * i;
            uint8_t type = (uint8_t)ld8(s + 4);
            const char *name;
            Stub fn;
            if ((type & 0xe0) || (type & 0x0e) != 0x0e || !(type & 1)) continue;
            name = (const char *)(uintptr_t)(base + stroff + ld32(s));
            if (name[0] == '_' && (fn = name_verify(name + 1)) != 0) { addr_add(ld32(s + 8) + vmslide, fn); n++; }
        }
        if (getenv("ROSETTA_STATS")) fprintf(stderr, "rosetta: native replacements in %08x: %u\n", head, n);
        return;
    }
    if (!libsystem) return;
    if (uuid && store_read(uuid, head, &n)) {
        if (getenv("ROSETTA_STATS")) fprintf(stderr, "rosetta: libSystem at %08x, %u stubs from the store, after %.2f ms\n", head, n, bl_ms());
        return;
    }
    /* read only the external symbols (LC_DYSYMTAB), not the local ones before them. Binary search (they are sorted)
       was slower: 2000 comparisons at scattered places versus one contiguous pass */
    if (next && iext + next <= nsyms) { i = iext; nsyms = iext + next; }
    else i = 0;
    g_new_n = 0;
    for (; i < nsyms; i++) {
        uint32_t s = base + symoff + 12 * i;
        uint8_t type = (uint8_t)ld8(s + 4);
        const char *name;
        Stub fn;
        if ((type & 0xe0) || (type & 0x0e) != 0x0e || !(type & 1)) continue;   /* only N_SECT | N_EXT */
        name = (const char *)(uintptr_t)(base + stroff + ld32(s));
        if (name[0] != '_') continue;
        fn = name_verify(name + 1);
        if (fn) {
            addr_add(ld32(s + 8) + vmslide, fn);
            n++;
            if (g_new_n < STORE_MAX) { g_new[g_new_n].offset = ld32(s + 8) + vmslide - head; g_new[g_new_n].fn = fn; g_new_n++; }
        }
    }
    if (uuid) store_write(uuid);
    if (getenv("ROSETTA_STATS")) fprintf(stderr, "rosetta: libSystem at %08x, %u stubs, after %.2f ms\n", head, n, bl_ms());
}

static unsigned long g_st_th_find, g_st_th_loop, g_st_th_region;
static uint64_t g_st_th_time;
static int g_st_th_error;

static int pagefault(void)
{
    task_events_info_data_t ev;
    mach_msg_type_number_t n = TASK_EVENTS_INFO_COUNT;
    memset(&ev, 0, sizeof ev);
    task_info(mach_task_self(), TASK_EVENTS_INFO, (task_info_t)&ev, &n);
    return ev.faults;
}

void thunk_stats(void)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    fprintf(stderr, "rosetta: thunk_find %lu times (%lu image list passes, %lu vm_region), %d images, "
            "reading images %.2f ms (%d page faults), %lu symbol stubs resolved on demand\n", g_st_th_find, g_st_th_loop,
            g_st_th_region, g_nimage, (double)g_st_th_time * tb.numer / tb.denom / 1e6, g_st_th_error, g_st_stubs_resolved);
}

Stub thunk_find(uint32_t eip)
{
    int i;
    vm_address_t addr;
    vm_size_t sz;
    vm_region_basic_info_data_t info;
    mach_msg_type_number_t n = VM_REGION_BASIC_INFO_COUNT;
    mach_port_t obj;
    Stub gt = gl_tramp(eip);                    /* dispatch table of a guest OpenGL context */
    if (gt) return gt;
    if (g_off < 0) g_off = getenv("ROSETTA_NO_STUB") != 0;
    if (g_off) return 0;
    if (!g_names_done) names_build();
    g_st_th_find++;
    if (g_last < g_nimage && eip - g_image[g_last].start < g_image[g_last].end - g_image[g_last].start)
        return g_image[g_last].libsystem ? addr_find(eip) : 0;
    g_st_th_loop++;
    for (i = 0; i < g_nimage; i++)
        if (eip - g_image[i].start < g_image[i].end - g_image[i].start) {
            g_last = i;
            return g_image[i].libsystem ? addr_find(eip) : 0;
        }
    g_st_th_region++;
    addr = eip;
    if (vm_region(mach_task_self(), &addr, &sz, VM_REGION_BASIC_INFO, (vm_region_info_t)&info, &n, &obj) || addr > eip)
        return 0;
    if ((info.protection & VM_PROT_READ) && ld32((uint32_t)addr) == 0xfeedface && ld32((uint32_t)addr + 4) == 7)
    {
        uint64_t t0 = mach_absolute_time();
        int f0 = pagefault();
        g_bl_t0 = t0;
        image_read((uint32_t)addr, (uint32_t)(addr + sz));
        g_st_th_time += mach_absolute_time() - t0;
        g_st_th_error += pagefault() - f0;
    }
    else image_note((uint32_t)addr, (uint32_t)(addr + sz), 0);
    for (i = 0; i < g_nimage; i++)
        if (eip - g_image[i].start < g_image[i].end - g_image[i].start) return g_image[i].libsystem ? addr_find(eip) : 0;
    return 0;
}

void thunk_forget(void)
{
    g_nimage = 0;
    g_last = 0;
    memset(g_addr, 0, sizeof g_addr);
    memset(g_stub, 0, sizeof g_stub);
    g_nsa = 0;
}

const char *thunk_name(Stub fn)
{
    unsigned i;
    for (i = 0; i < LIST; i++) if (g_list[i].fn == fn) return g_list[i].name;
    return gl_name(fn) ? gl_name(fn) : "?";
}

char thunk_kind(Stub fn, void **native)
{
    unsigned i;
    for (i = 0; i < LIST; i++) if (g_list[i].fn == fn) { *native = g_list[i].native; return g_list[i].kind; }
    *native = 0;
    return 'g';
}
