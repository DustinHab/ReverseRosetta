/* ca.c - CoreAudio's device layer (HAL) for Intel programs: the system's PowerPC HAL instead of the i386 one.
 *
 * The i386 HAL cannot reach the sound hardware of a PowerPC Mac: the kernel opens the audio engine's user client only
 * for a client of its own byte order (io_service_open_extended passes the client's NDR record; a cross-endian client
 * is refused with kIOReturnUnsupported unless the driver allows it), and the engine's shared buffers are big-endian
 * anyway. So thunk.c replaces the HAL functions of the guest's CoreAudio image by name with the functions here
 * (ca_stub); the rest of CoreAudio, AudioToolbox and the AUHAL component stay i386 code on top of them.
 *
 * Property data is converted by its type (four-character selector): 32-bit words (default), Float64 values and ranges,
 * stream descriptions (Float64 sample rate, then words; the big-endian flag of linear PCM is dropped towards the guest
 * and set towards the host, the samples are swapped instead), C strings as they are. Properties that carry CF objects
 * or pointers (CFString names and UIDs, value translations) are refused with kAudioHardwareUnknownPropertyError; the
 * run loop property is accepted and ignored. IOProcs: the host HAL calls ca_ioproc on its I/O thread, which copies time
 * stamps and buffers into guest memory (swapping the samples), runs the program's IOProc through gcall.c and swaps
 * its output back. Listeners go the same way. The thread lock is given away around every call into the host HAL: its
 * I/O thread may wait for the guest (AudioDeviceStop waits for a running IOProc).
 * ROSETTA_CA_LOG=1 shows every call. */
#include "ca.h"
#include "gcall.h"
#include "thread.h"
#include "cpu.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

typedef int32_t OSStatus_;
#define ERR_UNKNOWN_PROPERTY 0x77686f3fu    /* 'who?' kAudioHardwareUnknownPropertyError */
#define ERR_BAD_PROPERTY_SIZE 0x2173697au   /* '!siz' */
#define ERR_ILLEGAL_OPERATION 0x6e6f7065u   /* 'nope' */
#define ERR_BAD_DEVICE 0x21646576u          /* '!dev' */

#define A(k) ld32(c->r[ESP] + 4 + 4 * (k))

static int g_ca_log = -1;
static void ca_log(Cpu *c, const char *f)
{
    if (g_ca_log < 0) g_ca_log = getenv("ROSETTA_CA_LOG") != 0;
    if (g_ca_log) fprintf(stderr, "[ca] %s(%08x %08x %08x %08x %08x) = %d\n", f + 2, A(0), A(1), A(2), A(3), A(4), (int)c->r[EAX]);
}
#define RET(v) do { c->r[EAX] = (uint32_t)(v); ca_log(c, __func__); return 0; } while (0)

/* ---------- the host's HAL ---------- */

typedef struct { uint32_t sel, scope, element; } Addr;
typedef OSStatus_ (*IOProcH)(uint32_t, const void *, const void *, const void *, void *, const void *, void *);

static void *g_ca;
static void *ca_sym(const char *n)
{
    void *f;
    if (!g_ca && !(g_ca = dlopen("/System/Library/Frameworks/CoreAudio.framework/CoreAudio", RTLD_LAZY | RTLD_LOCAL))) {
        fprintf(stderr, "rosetta: CoreAudio: %s\n", dlerror());
        return 0;
    }
    if (!(f = dlsym(g_ca, n))) fprintf(stderr, "rosetta: CoreAudio: no %s on this system\n", n);
    return f;
}
static void *ca_set(void **p, const char *n) { return *p = ca_sym(n); }
#define SYM(n) (f_##n ? f_##n : ca_set(&f_##n, #n))
#define F(n) static void *f_##n
F(AudioObjectGetPropertyData); F(AudioObjectGetPropertyDataSize); F(AudioObjectSetPropertyData);
F(AudioObjectHasProperty); F(AudioObjectIsPropertySettable); F(AudioObjectAddPropertyListener);
F(AudioObjectRemovePropertyListener); F(AudioDeviceCreateIOProcID); F(AudioDeviceDestroyIOProcID);
F(AudioDeviceStart); F(AudioDeviceStop); F(AudioDeviceGetCurrentTime); F(AudioDeviceTranslateTime);
F(AudioDeviceGetNearestStartTime); F(AudioDeviceStartAtTime);
#undef F

/* calls into the host HAL without the thread lock */
#define UNLOCKED(expr) ({ Cpu *me_ = glock_release(); OSStatus_ r_ = (expr); glock_acquire(me_); r_; })

/* ---------- property data ---------- */

#define FOUR(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

/* kind of a property's data: 'w' 32-bit words, 'd' Float64 values, 'A' stream descriptions (40 bytes), 'R' ranged
   descriptions (56), 's' bytes, 'u' IOProc stream usage, 'n' run loop (ignored), 'x' not convertible */
static int prop_kind(uint32_t sel)
{
    switch (sel) {
    case FOUR('n','s','r','t'): case FOUR('n','s','r','#'): case FOUR('f','s','z','#'): case FOUR('b','s','z','#'):
    case FOUR('v','d','b','#'): case FOUR('a','s','r','t'):
        return 'd';
    case FOUR('s','f','m','t'): case FOUR('p','f','t',' '): case FOUR('s','f','m','a'): case FOUR('p','f','t','m'):
    case FOUR('s','f','m','#'): case FOUR('p','f','t','#'):
        return 'A';
    case FOUR('s','f','a','#'): case FOUR('p','f','t','a'):
        return 'R';
    case FOUR('n','a','m','e'): case FOUR('m','a','k','r'): case FOUR('c','h','n','m'):
        return 's';
    case FOUR('s','u','s','e'):
        return 'u';
    case FOUR('r','n','l','p'):
        return 'n';
    case FOUR('l','n','a','m'): case FOUR('l','m','a','k'): case FOUR('u','i','d',' '): case FOUR('m','u','i','d'):
    case FOUR('l','c','h','n'): case FOUR('l','c','a','t'): case FOUR('l','n','u','m'): case FOUR('c','a','p','p'):
    case FOUR('d','u','i','d'): case FOUR('u','i','d','d'): case FOUR('l','s','c','n'): case FOUR('s','s','c','n'):
    case FOUR('c','s','c','n'): case FOUR('l','c','s','n'): case FOUR('p','i','b','i'): case FOUR('b','o','x','d'):
    case FOUR('i','c','o','n'): case FOUR('t','r','a','n'):
        return 'x';
    }
    return 'w';
}

static uint64_t swap64(uint64_t v) { return (uint64_t)swap32((uint32_t)v) << 32 | swap32((uint32_t)(v >> 32)); }

/* one stream description (40 bytes) between the byte orders, in place: the Float64 sample rate, then eight words */
static void asbd_swap(uint8_t *p)
{
    uint32_t *w = (uint32_t *)(void *)p, k;
    uint64_t *q = (uint64_t *)(void *)p;
    q[0] = swap64(q[0]);
    for (k = 2; k < 10; k++) w[k] = swap32(w[k]);
}

static uint32_t host_flag_fix(uint32_t id, uint32_t flags, uint32_t bits, int to_guest)
{
    if (id != FOUR('l','p','c','m') || bits <= 8) return flags;
    return to_guest ? flags & ~2u : flags | 2u;                 /* kAudioFormatFlagIsBigEndian */
}

/* data of selector sel between host and guest form, in place (n bytes) */
static void prop_convert(uint32_t sel, uint8_t *p, uint32_t n, int to_guest)
{
    uint32_t k, *w = (uint32_t *)(void *)p;
    switch (prop_kind(sel)) {
    case 's': case 'n': case 'x':
        return;
    case 'd':
        for (k = 0; k + 8 <= n; k += 8) *(uint64_t *)(void *)(p + k) = swap64(*(uint64_t *)(void *)(p + k));
        return;
    case 'A': case 'R': {
        uint32_t step = prop_kind(sel) == 'A' ? 40 : 56;
        for (k = 0; k + step <= n; k += step) {
            uint32_t *d = (uint32_t *)(void *)(p + k), id, fl, bits;
            if (to_guest) {                 /* host order -> fix the flag -> guest order */
                id = d[2]; fl = d[3]; bits = d[8];
                d[3] = host_flag_fix(id, fl, bits, 1);
                asbd_swap(p + k);
            } else {                        /* guest order -> host order -> fix the flag */
                asbd_swap(p + k);
                d[3] = host_flag_fix(d[2], d[3], d[8], 0);
            }
            if (step == 56) {
                *(uint64_t *)(void *)(p + k + 40) = swap64(*(uint64_t *)(void *)(p + k + 40));
                *(uint64_t *)(void *)(p + k + 48) = swap64(*(uint64_t *)(void *)(p + k + 48));
            }
        }
        return;
    }
    }
    for (k = 0; k + 4 <= n; k += 4) w[k / 4] = swap32(w[k / 4]);
}

/* scratch buffer for property data (one guest call at a time per thread: by the lock) */
static uint8_t *g_buf;
static uint32_t g_buf_size;
static uint8_t *buf(uint32_t n)
{
    if (n > (1u << 24)) return 0;
    if (n > g_buf_size) {
        void *p = realloc(g_buf, n ? n : 1);
        if (!p) return 0;
        g_buf = p;
        g_buf_size = n;
    }
    return g_buf;
}

/* ---------- IOProcs ---------- */

typedef struct IoEntry {
    struct IoEntry *next;
    uint32_t dev, proc, client;             /* the guest's */
    void *host_id;                          /* AudioDeviceIOProcID of the host */
    int old_api;                            /* registered with AudioDeviceAddIOProc (identified by proc) */
    uint8_t *gmem;                          /* guest memory for time stamps and buffer lists */
    uint32_t gsize;
} IoEntry;
static IoEntry *g_io;
static pthread_mutex_t g_io_m = PTHREAD_MUTEX_INITIALIZER;

static void ts_to_guest(uint8_t *g, const uint8_t *h)
{
    int k;
    if (!h) { memset(g, 0, 64); return; }
    for (k = 0; k < 4; k++) *(uint64_t *)(void *)(g + 8 * k) = swap64(*(const uint64_t *)(const void *)(h + 8 * k));
    st16((uint32_t)(uintptr_t)g + 32, *(const uint16_t *)(const void *)(h + 32));
    st16((uint32_t)(uintptr_t)g + 34, *(const uint16_t *)(const void *)(h + 34));
    for (k = 36; k < 48; k += 4) st32((uint32_t)(uintptr_t)g + (uint32_t)k, *(const uint32_t *)(const void *)(h + k));
    for (k = 48; k < 56; k += 2) st16((uint32_t)(uintptr_t)g + (uint32_t)k, *(const uint16_t *)(const void *)(h + k));
    st32((uint32_t)(uintptr_t)g + 56, *(const uint32_t *)(const void *)(h + 56));
    st32((uint32_t)(uintptr_t)g + 60, *(const uint32_t *)(const void *)(h + 60));
}

static void ts_to_host(uint8_t *h, uint32_t g)
{
    int k;
    for (k = 0; k < 4; k++) *(uint64_t *)(void *)(h + 8 * k) = ld64(g + 8 * (uint32_t)k);
    *(uint16_t *)(void *)(h + 32) = (uint16_t)ld16(g + 32);
    *(uint16_t *)(void *)(h + 34) = (uint16_t)ld16(g + 34);
    for (k = 36; k < 48; k += 4) *(uint32_t *)(void *)(h + k) = ld32(g + (uint32_t)k);
    for (k = 48; k < 56; k += 2) *(uint16_t *)(void *)(h + k) = (uint16_t)ld16(g + (uint32_t)k);
    *(uint32_t *)(void *)(h + 56) = ld32(g + 56);
    *(uint32_t *)(void *)(h + 60) = ld32(g + 60);
}

/* bytes a buffer list with its data needs in guest memory */
static uint32_t abl_need(const uint32_t *l)
{
    uint32_t n, k, s;
    if (!l) return 0;
    n = l[0];
    s = 4 + 12 * n;
    for (k = 0; k < n; k++) s += (l[1 + 3 * k + 1] + 15) & ~15u;
    return s;
}

/* host buffer list -> guest copy at g (samples swapped as 32-bit words if copy, else zero); returns bytes used */
static uint32_t abl_to_guest(uint8_t *g, const uint32_t *l, int copy)
{
    uint32_t n = l[0], k, data = 4 + 12 * n, ga = (uint32_t)(uintptr_t)g;
    st32(ga, n);
    for (k = 0; k < n; k++) {
        uint32_t ch = l[1 + 3 * k], sz = l[1 + 3 * k + 1], i;
        const uint32_t *src = (const uint32_t *)(uintptr_t)l[1 + 3 * k + 2];
        st32(ga + 4 + 12 * k, ch);
        st32(ga + 8 + 12 * k, sz);
        st32(ga + 12 + 12 * k, ga + data);
        if (copy && src) for (i = 0; i + 4 <= sz; i += 4) st32(ga + data + i, src[i / 4]);   /* st32: guest order */
        else memset(g + data, 0, sz);
        data += (sz + 15) & ~15u;
    }
    return data;
}

/* guest output -> host buffers (swap) */
static void abl_to_host(uint32_t *l, uint32_t ga)
{
    uint32_t n = l[0], k;
    if (ld32(ga) < n) n = ld32(ga);
    for (k = 0; k < n; k++) {
        uint32_t sz = l[1 + 3 * k + 1], gsz = ld32(ga + 8 + 12 * k), gd = ld32(ga + 12 + 12 * k), i;
        uint32_t *dst = (uint32_t *)(uintptr_t)l[1 + 3 * k + 2];
        if (gsz < sz) sz = gsz;
        if (!dst || !gd) continue;
        for (i = 0; i + 4 <= sz; i += 4) dst[i / 4] = ld32(gd + i);
    }
}

/* the host HAL's I/O thread: the program's IOProc in the guest */
static OSStatus_ ca_ioproc(uint32_t dev, const void *now, const void *in, const void *in_time, void *out,
                           const void *out_time, void *client)
{
    IoEntry *e = (IoEntry *)client;
    uint32_t need = 3 * 64 + abl_need((const uint32_t *)in) + abl_need((const uint32_t *)out) + 64, ga, gin = 0, gout = 0;
    uint32_t args[7], r = 0;
    uint8_t *g;
    if (need > e->gsize) {
        void *p = mmap(0, (need + 4095) & ~4095u, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (p == MAP_FAILED) return 0;
        if (e->gmem) munmap(e->gmem, e->gsize);
        e->gmem = p;
        e->gsize = (need + 4095) & ~4095u;
    }
    g = e->gmem;
    ga = (uint32_t)(uintptr_t)g;
    ts_to_guest(g, (const uint8_t *)now);
    ts_to_guest(g + 64, (const uint8_t *)in_time);
    ts_to_guest(g + 128, (const uint8_t *)out_time);
    {
        uint32_t off = 192;
        if (in) { gin = ga + off; off += abl_to_guest(g + off, (const uint32_t *)in, 1); }
        if (out) { gout = ga + off; abl_to_guest(g + off, (const uint32_t *)out, 0); }
    }
    args[0] = dev; args[1] = ga; args[2] = gin; args[3] = in_time ? ga + 64 : 0; args[4] = gout;
    args[5] = out_time ? ga + 128 : 0; args[6] = e->client;
    if (guest_call(0, e->proc, args, 7, &r, 200)) return 0;      /* too late: this cycle stays silent */
    if (out && gout) abl_to_host((uint32_t *)out, gout);
    return (OSStatus_)r;
}

static IoEntry *io_find(uint32_t dev, uint32_t proc, int by_entry)
{
    IoEntry *e;
    for (e = g_io; e; e = e->next)
        if (e->dev == dev && (by_entry ? (uint32_t)(uintptr_t)e == proc : e->old_api && e->proc == proc)) return e;
    return 0;
}

static IoEntry *io_new(Cpu *c, uint32_t dev, uint32_t proc, uint32_t client, int old_api, OSStatus_ *err)
{
    IoEntry *e;
    if (gcall_start(c)) { *err = ERR_ILLEGAL_OPERATION; return 0; }
    if (!(e = calloc(1, sizeof *e))) { *err = ERR_ILLEGAL_OPERATION; return 0; }
    e->dev = dev; e->proc = proc; e->client = client; e->old_api = old_api;
    *err = UNLOCKED(((OSStatus_ (*)(uint32_t, IOProcH, void *, void **))SYM(AudioDeviceCreateIOProcID))(dev, (IOProcH)ca_ioproc, e, &e->host_id));
    if (*err) { free(e); return 0; }
    pthread_mutex_lock(&g_io_m);
    e->next = g_io;
    g_io = e;
    pthread_mutex_unlock(&g_io_m);
    return e;
}

static void io_free(IoEntry *e)
{
    IoEntry **p;
    pthread_mutex_lock(&g_io_m);
    for (p = &g_io; *p; p = &(*p)->next) if (*p == e) { *p = e->next; break; }
    pthread_mutex_unlock(&g_io_m);
    if (e->gmem) munmap(e->gmem, e->gsize);
    free(e);
}

/* the host id for the guest's proc argument of Start/Stop: entry (new API) or proc (old) */
static void *io_host(uint32_t dev, uint32_t proc)
{
    IoEntry *e;
    if (!proc) return 0;
    if ((e = io_find(dev, proc, 1)) || (e = io_find(dev, proc, 0))) return e->host_id;
    return (void *)-1;
}

/* ---------- listeners ---------- */

typedef struct Listener {
    struct Listener *next;
    int kind;                               /* 'o' object (AudioObject), 'd' device, 'h' hardware */
    uint32_t obj, proc, client;
    Addr a;
    int input;
    uint8_t *gmem;                          /* guest copy of the addresses ('o') */
} Listener;
static Listener *g_listen;

static OSStatus_ ca_listener_object(uint32_t obj, uint32_t n, const Addr *addrs, void *client)
{
    Listener *l = (Listener *)client;
    uint32_t args[4], r = 0, k;
    if (!l->gmem && !(l->gmem = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0))) return 0;
    if (l->gmem == MAP_FAILED) { l->gmem = 0; return 0; }
    if (n > 300) n = 300;
    for (k = 0; k < n; k++) {
        st32((uint32_t)(uintptr_t)l->gmem + 12 * k, addrs[k].sel);
        st32((uint32_t)(uintptr_t)l->gmem + 12 * k + 4, addrs[k].scope);
        st32((uint32_t)(uintptr_t)l->gmem + 12 * k + 8, addrs[k].element);
    }
    args[0] = obj; args[1] = n; args[2] = (uint32_t)(uintptr_t)l->gmem; args[3] = l->client;
    guest_call(0, l->proc, args, 4, &r, 2000);
    return (OSStatus_)r;
}

/* old device and hardware listeners through the object API: the guest's proc gets its own argument list */
static OSStatus_ ca_listener_old(uint32_t obj, uint32_t n, const Addr *addrs, void *client)
{
    Listener *l = (Listener *)client;
    uint32_t args[5], r = 0, k;
    for (k = 0; k < n; k++) {
        if (l->kind == 'h') { args[0] = addrs[k].sel; args[1] = l->client; guest_call(0, l->proc, args, 2, &r, 2000); }
        else {
            args[0] = obj; args[1] = addrs[k].element; args[2] = addrs[k].scope == FOUR('i','n','p','t');
            args[3] = addrs[k].sel; args[4] = l->client;
            guest_call(0, l->proc, args, 5, &r, 2000);
        }
    }
    return (OSStatus_)r;
}

static OSStatus_ listen_add(Cpu *c, int kind, uint32_t obj, const Addr *a, uint32_t proc, uint32_t client)
{
    Listener *l;
    OSStatus_ e;
    if (gcall_start(c)) return ERR_ILLEGAL_OPERATION;
    if (!(l = calloc(1, sizeof *l))) return ERR_ILLEGAL_OPERATION;
    l->kind = kind; l->obj = obj; l->a = *a; l->proc = proc; l->client = client;
    e = UNLOCKED(((OSStatus_ (*)(uint32_t, const Addr *, void *, void *))SYM(AudioObjectAddPropertyListener))(
        obj, a, kind == 'o' ? (void *)ca_listener_object : (void *)ca_listener_old, l));
    if (e) { free(l); return e; }
    l->next = g_listen;
    g_listen = l;
    return 0;
}

static OSStatus_ listen_remove(int kind, uint32_t obj, const Addr *a, uint32_t proc, uint32_t client, int match_client)
{
    Listener **p, *l;
    for (p = &g_listen; (l = *p) != 0; p = &l->next)
        if (l->kind == kind && l->obj == obj && l->proc == proc && (!match_client || l->client == client) &&
            l->a.sel == a->sel && l->a.scope == a->scope && l->a.element == a->element) {
            OSStatus_ e = UNLOCKED(((OSStatus_ (*)(uint32_t, const Addr *, void *, void *))SYM(AudioObjectRemovePropertyListener))(
                obj, a, kind == 'o' ? (void *)ca_listener_object : (void *)ca_listener_old, l));
            *p = l->next;
            /* the listener's memory stays (a notification may be on its way) */
            return e;
        }
    return 0;
}

/* ---------- the object API ---------- */

static void addr_in(Addr *a, uint32_t g) { a->sel = ld32(g); a->scope = ld32(g + 4); a->element = ld32(g + 8); }

/* get: data of the guest's size (ioSize at gsize) into gdata */
static OSStatus_ get_data(uint32_t obj, const Addr *a, uint32_t qsize, uint32_t qdata, uint32_t gsize, uint32_t gdata)
{
    uint32_t n = ld32(gsize), k;
    uint8_t *p, *q = 0;
    OSStatus_ e;
    int kind = prop_kind(a->sel);
    if (kind == 'x' || kind == 'n') return ERR_UNKNOWN_PROPERTY;
    if (!(p = buf(n + qsize))) return ERR_BAD_PROPERTY_SIZE;
    if (qsize && qdata) {                    /* qualifier: words (device ids, class ids) */
        q = p + n;
        for (k = 0; k + 4 <= qsize; k += 4) *(uint32_t *)(void *)(q + k) = ld32(qdata + k);
    }
    e = UNLOCKED(((OSStatus_ (*)(uint32_t, const Addr *, uint32_t, const void *, uint32_t *, void *))SYM(AudioObjectGetPropertyData))(
        obj, a, qsize, q, &n, p));
    if (e) return e;
    if (kind == 'u' && n >= 4) {             /* stream usage: the host id back to the guest's entry */
        IoEntry *x;
        for (x = g_io; x; x = x->next) if (x->host_id == *(void **)(void *)p) break;
        st32(gdata, x ? (x->old_api ? x->proc : (uint32_t)(uintptr_t)x) : 0);
        for (k = 4; k + 4 <= n; k += 4) st32(gdata + k, *(uint32_t *)(void *)(p + k));
        st32(gsize, n);
        return 0;
    }
    prop_convert(a->sel, p, n, 1);
    memcpy((void *)(uintptr_t)gdata, p, n);
    st32(gsize, n);
    return 0;
}

static OSStatus_ set_data(uint32_t obj, const Addr *a, uint32_t qsize, uint32_t qdata, uint32_t size, uint32_t gdata)
{
    uint8_t *p;
    uint32_t k;
    int kind = prop_kind(a->sel);
    if (kind == 'n') return 0;               /* run loop: the host HAL keeps its own thread for notifications */
    if (kind == 'x') return ERR_UNKNOWN_PROPERTY;
    if (!(p = buf(size + qsize + 4))) return ERR_BAD_PROPERTY_SIZE;
    memcpy(p, (const void *)(uintptr_t)gdata, size);
    if (kind == 'u') {
        void *h = size >= 4 ? io_host(obj, ld32(gdata)) : 0;
        if (h == (void *)-1) return ERR_ILLEGAL_OPERATION;
        *(void **)(void *)p = h;
        for (k = 4; k + 4 <= size; k += 4) *(uint32_t *)(void *)(p + k) = ld32(gdata + k);
    } else prop_convert(a->sel, p, size, 0);
    (void)qdata;
    return UNLOCKED(((OSStatus_ (*)(uint32_t, const Addr *, uint32_t, const void *, uint32_t, const void *))SYM(AudioObjectSetPropertyData))(
        obj, a, 0, 0, size, p));
}

static OSStatus_ get_size(uint32_t obj, const Addr *a, uint32_t qsize, uint32_t qdata, uint32_t *n)
{
    uint8_t q[64];
    uint32_t k;
    int kind = prop_kind(a->sel);
    if (kind == 'x' || kind == 'n') return ERR_UNKNOWN_PROPERTY;
    if (qsize > sizeof q) qsize = sizeof q;
    for (k = 0; k + 4 <= qsize; k += 4) *(uint32_t *)(void *)(q + k) = ld32(qdata + k);
    return UNLOCKED(((OSStatus_ (*)(uint32_t, const Addr *, uint32_t, const void *, uint32_t *))SYM(AudioObjectGetPropertyDataSize))(
        obj, a, qsize, qsize ? q : 0, n));
}

static int c_AudioObjectGetPropertyData(Cpu *c)
{
    Addr a;
    addr_in(&a, A(1));
    RET(get_data(A(0), &a, A(2), A(3), A(4), A(5)));
}

static int c_AudioObjectGetPropertyDataSize(Cpu *c)
{
    Addr a;
    uint32_t n = 0;
    OSStatus_ e;
    addr_in(&a, A(1));
    e = get_size(A(0), &a, A(2), A(3), &n);
    if (!e) st32(A(4), n);
    RET(e);
}

static int c_AudioObjectSetPropertyData(Cpu *c)
{
    Addr a;
    addr_in(&a, A(1));
    RET(set_data(A(0), &a, A(2), A(3), A(4), A(5)));
}

static int c_AudioObjectHasProperty(Cpu *c)
{
    Addr a;
    addr_in(&a, A(1));
    if (prop_kind(a.sel) == 'x') RET(0);
    RET(UNLOCKED(((OSStatus_ (*)(uint32_t, const Addr *))SYM(AudioObjectHasProperty))(A(0), &a)) & 0xff);
}

static int c_AudioObjectIsPropertySettable(Cpu *c)
{
    Addr a;
    unsigned char s = 0;
    OSStatus_ e;
    addr_in(&a, A(1));
    if (prop_kind(a.sel) == 'x') RET(ERR_UNKNOWN_PROPERTY);
    e = UNLOCKED(((OSStatus_ (*)(uint32_t, const Addr *, unsigned char *))SYM(AudioObjectIsPropertySettable))(A(0), &a, &s));
    if (!e) st8(A(2), s);
    RET(e);
}

static int c_AudioObjectAddPropertyListener(Cpu *c)
{
    Addr a;
    addr_in(&a, A(1));
    RET(listen_add(c, 'o', A(0), &a, A(2), A(3)));
}

static int c_AudioObjectRemovePropertyListener(Cpu *c)
{
    Addr a;
    addr_in(&a, A(1));
    RET(listen_remove('o', A(0), &a, A(2), A(3), 1));
}

/* ---------- the old device, stream and hardware API on top of it ---------- */

static Addr dev_addr(uint32_t channel, uint32_t input, uint32_t sel)
{
    Addr a;
    a.sel = sel;
    a.scope = (input & 0xff) ? FOUR('i','n','p','t') : FOUR('o','u','t','p');
    a.element = channel;
    return a;
}

static Addr glob_addr(uint32_t sel)
{
    Addr a;
    a.sel = sel; a.scope = FOUR('g','l','o','b'); a.element = 0;
    return a;
}

static int c_AudioDeviceGetProperty(Cpu *c)
{
    Addr a = dev_addr(A(1), A(2), A(3));
    RET(get_data(A(0), &a, 0, 0, A(4), A(5)));
}

static int c_AudioDeviceGetPropertyInfo(Cpu *c)
{
    Addr a = dev_addr(A(1), A(2), A(3));
    uint32_t n = 0;
    OSStatus_ e = get_size(A(0), &a, 0, 0, &n);
    if (!e && A(4)) st32(A(4), n);
    if (!e && A(5)) {
        unsigned char s = 0;
        UNLOCKED(((OSStatus_ (*)(uint32_t, const Addr *, unsigned char *))SYM(AudioObjectIsPropertySettable))(A(0), &a, &s));
        st8(A(5), s);
    }
    RET(e);
}

static int c_AudioDeviceSetProperty(Cpu *c)
{
    Addr a = dev_addr(A(2), A(3), A(4));     /* (device, when, channel, isInput, id, size, data) */
    RET(set_data(A(0), &a, 0, 0, A(5), A(6)));
}

static int c_AudioStreamGetProperty(Cpu *c)
{
    Addr a = dev_addr(A(1), 0, A(2));       /* (stream, channel, id, size, data) */
    a.scope = FOUR('g','l','o','b');
    RET(get_data(A(0), &a, 0, 0, A(3), A(4)));
}

static int c_AudioStreamGetPropertyInfo(Cpu *c)
{
    Addr a = dev_addr(A(1), 0, A(2));
    uint32_t n = 0;
    OSStatus_ e;
    a.scope = FOUR('g','l','o','b');
    e = get_size(A(0), &a, 0, 0, &n);
    if (!e && A(3)) st32(A(3), n);
    if (!e && A(4)) {
        unsigned char s = 0;
        UNLOCKED(((OSStatus_ (*)(uint32_t, const Addr *, unsigned char *))SYM(AudioObjectIsPropertySettable))(A(0), &a, &s));
        st8(A(4), s);
    }
    RET(e);
}

static int c_AudioStreamSetProperty(Cpu *c)
{
    Addr a = dev_addr(A(2), 0, A(3));       /* (stream, when, channel, id, size, data) */
    a.scope = FOUR('g','l','o','b');
    RET(set_data(A(0), &a, 0, 0, A(4), A(5)));
}

static int c_AudioHardwareGetProperty(Cpu *c)
{
    Addr a = glob_addr(A(0));
    RET(get_data(1, &a, 0, 0, A(1), A(2)));   /* kAudioObjectSystemObject */
}

static int c_AudioHardwareGetPropertyInfo(Cpu *c)
{
    Addr a = glob_addr(A(0));
    uint32_t n = 0;
    OSStatus_ e = get_size(1, &a, 0, 0, &n);
    if (!e && A(1)) st32(A(1), n);
    if (!e && A(2)) {
        unsigned char s = 0;
        UNLOCKED(((OSStatus_ (*)(uint32_t, const Addr *, unsigned char *))SYM(AudioObjectIsPropertySettable))(1, &a, &s));
        st8(A(2), s);
    }
    RET(e);
}

static int c_AudioHardwareSetProperty(Cpu *c)
{
    Addr a = glob_addr(A(0));
    RET(set_data(1, &a, 0, 0, A(1), A(2)));
}

static int c_AudioDeviceAddPropertyListener(Cpu *c)
{
    Addr a = dev_addr(A(1), A(2), A(3));     /* (device, channel, isInput, id, proc, client) */
    RET(listen_add(c, 'd', A(0), &a, A(4), A(5)));
}

static int c_AudioDeviceRemovePropertyListener(Cpu *c)
{
    Addr a = dev_addr(A(1), A(2), A(3));
    RET(listen_remove('d', A(0), &a, A(4), 0, 0));
}

static int c_AudioHardwareAddPropertyListener(Cpu *c)
{
    Addr a = glob_addr(A(0));
    RET(listen_add(c, 'h', 1, &a, A(1), A(2)));
}

static int c_AudioHardwareRemovePropertyListener(Cpu *c)
{
    Addr a = glob_addr(A(0));
    RET(listen_remove('h', 1, &a, A(1), 0, 0));
}

/* ---------- IOProcs and time ---------- */

static int c_AudioDeviceAddIOProc(Cpu *c)
{
    OSStatus_ e = 0;
    if (io_find(A(0), A(1), 0)) RET(0);
    io_new(c, A(0), A(1), A(2), 1, &e);
    RET(e);
}

static int c_AudioDeviceRemoveIOProc(Cpu *c)
{
    IoEntry *e = io_find(A(0), A(1), 0);
    OSStatus_ r;
    if (!e) RET(ERR_ILLEGAL_OPERATION);
    r = UNLOCKED(((OSStatus_ (*)(uint32_t, void *))SYM(AudioDeviceDestroyIOProcID))(A(0), e->host_id));
    io_free(e);
    RET(r);
}

static int c_AudioDeviceCreateIOProcID(Cpu *c)
{
    OSStatus_ e = 0;
    IoEntry *x = io_new(c, A(0), A(1), A(2), 0, &e);
    if (x) st32(A(3), (uint32_t)(uintptr_t)x);
    RET(e);
}

static int c_AudioDeviceDestroyIOProcID(Cpu *c)
{
    IoEntry *e = io_find(A(0), A(1), 1);
    OSStatus_ r;
    if (!e) RET(ERR_ILLEGAL_OPERATION);
    r = UNLOCKED(((OSStatus_ (*)(uint32_t, void *))SYM(AudioDeviceDestroyIOProcID))(A(0), e->host_id));
    io_free(e);
    RET(r);
}

static int c_AudioDeviceStart(Cpu *c)
{
    void *h = io_host(A(0), A(1));
    if (h == (void *)-1) RET(ERR_ILLEGAL_OPERATION);
    RET(UNLOCKED(((OSStatus_ (*)(uint32_t, void *))SYM(AudioDeviceStart))(A(0), h)));
}

static int c_AudioDeviceStop(Cpu *c)
{
    void *h = io_host(A(0), A(1));
    if (h == (void *)-1) RET(ERR_ILLEGAL_OPERATION);
    RET(UNLOCKED(((OSStatus_ (*)(uint32_t, void *))SYM(AudioDeviceStop))(A(0), h)));
}

static int c_AudioDeviceGetCurrentTime(Cpu *c)
{
    uint8_t t[64];
    OSStatus_ e;
    memset(t, 0, sizeof t);
    e = UNLOCKED(((OSStatus_ (*)(uint32_t, void *))SYM(AudioDeviceGetCurrentTime))(A(0), t));
    if (!e) ts_to_guest((uint8_t *)(uintptr_t)A(1), t);
    RET(e);
}

static int c_AudioDeviceTranslateTime(Cpu *c)
{
    uint8_t in[64], out[64];
    OSStatus_ e;
    ts_to_host(in, A(1));
    ts_to_host(out, A(2));                    /* the flags say what is wanted */
    e = UNLOCKED(((OSStatus_ (*)(uint32_t, const void *, void *))SYM(AudioDeviceTranslateTime))(A(0), in, out));
    if (!e) ts_to_guest((uint8_t *)(uintptr_t)A(2), out);
    RET(e);
}

static int c_AudioDeviceGetNearestStartTime(Cpu *c)
{
    uint8_t t[64];
    OSStatus_ e;
    ts_to_host(t, A(1));
    e = UNLOCKED(((OSStatus_ (*)(uint32_t, void *, uint32_t))SYM(AudioDeviceGetNearestStartTime))(A(0), t, A(2)));
    if (!e) ts_to_guest((uint8_t *)(uintptr_t)A(1), t);
    RET(e);
}

static int c_AudioDeviceStartAtTime(Cpu *c)
{
    uint8_t t[64];
    void *h = io_host(A(0), A(1));
    OSStatus_ e;
    if (h == (void *)-1) RET(ERR_ILLEGAL_OPERATION);
    ts_to_host(t, A(2));
    e = UNLOCKED(((OSStatus_ (*)(uint32_t, void *, void *, uint32_t))SYM(AudioDeviceStartAtTime))(A(0), h, t, A(3)));
    if (!e) ts_to_guest((uint8_t *)(uintptr_t)A(2), t);
    RET(e);
}

/* ---------- names ---------- */

#define E(n) { #n, c_##n }
static const struct { const char *name; Stub fn; } g_ca_list[] = {
    E(AudioObjectGetPropertyData), E(AudioObjectGetPropertyDataSize), E(AudioObjectSetPropertyData),
    E(AudioObjectHasProperty), E(AudioObjectIsPropertySettable), E(AudioObjectAddPropertyListener),
    E(AudioObjectRemovePropertyListener), E(AudioDeviceGetProperty), E(AudioDeviceGetPropertyInfo),
    E(AudioDeviceSetProperty), E(AudioStreamGetProperty), E(AudioStreamGetPropertyInfo), E(AudioStreamSetProperty),
    E(AudioHardwareGetProperty), E(AudioHardwareGetPropertyInfo), E(AudioHardwareSetProperty),
    E(AudioDeviceAddPropertyListener), E(AudioDeviceRemovePropertyListener), E(AudioHardwareAddPropertyListener),
    E(AudioHardwareRemovePropertyListener), E(AudioDeviceAddIOProc), E(AudioDeviceRemoveIOProc),
    E(AudioDeviceCreateIOProcID), E(AudioDeviceDestroyIOProcID), E(AudioDeviceStart), E(AudioDeviceStop),
    E(AudioDeviceGetCurrentTime), E(AudioDeviceTranslateTime), E(AudioDeviceGetNearestStartTime),
    E(AudioDeviceStartAtTime),
};
#undef E
#define CA_N (sizeof g_ca_list / sizeof g_ca_list[0])

Stub ca_stub(const char *s, size_t n)
{
    size_t k;
    if (n < 10 || memcmp(s, "Audio", 5)) return 0;
    for (k = 0; k < CA_N; k++)
        if (strlen(g_ca_list[k].name) == n && !memcmp(g_ca_list[k].name, s, n)) return g_ca_list[k].fn;
    return 0;
}
