/* mach_msg.c - Mach traps of the guest, Mach messages in both byte orders.
 *
 * The kernel reads header, descriptors and trailer itself; the translator converts those (header: 6 words;
 * descriptors 12 bytes: swap the first two words, the third with the bit fields is byte for byte the same in both
 * byte orders; trailer: words). MIG converts the content of MIG messages itself: every message carries an NDR
 * record with the byte order of the sender (Intel: int_rep 1), server and client stubs swap integers when it
 * differs from their own (built that way for Rosetta). OOL port lists (names) are converted here, OOL memory stays
 * raw.
 */
#include "mach_msg.h"
#include "interp.h"
#include "sys.h"
#include "thread.h"
#include "jit.h"
#include "gl.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_traps.h>
#include <mach/thread_switch.h>

/* in 10.5 without a public header */
extern mach_port_name_t mk_timer_create(void);
extern kern_return_t mk_timer_destroy(mach_port_name_t name);
extern kern_return_t mk_timer_arm(mach_port_name_t name, uint64_t deadline);
extern kern_return_t mk_timer_cancel(mach_port_name_t name, uint64_t *deadline);
extern mach_port_name_t mach_reply_port(void);
extern kern_return_t clock_sleep_trap(mach_port_name_t clock, sleep_type_t type, int sec, int nsec, mach_timespec_t *wakeup);
extern kern_return_t map_fd(int fd, vm_offset_t offset, vm_offset_t *va, boolean_t findspace, vm_size_t size);

static uint32_t g_base;
static inline uint32_t arg(int i) { return ld32(g_base + 4 + 4 * (uint32_t)i); }
static inline uint64_t arg64(int i) { return (uint64_t)arg(i) | (uint64_t)arg(i + 1) << 32; }

static inline uint32_t w_read(const uint8_t *p, uint32_t o) { return *(const uint32_t *)(p + o); }
static inline void w_swap(uint8_t *p, uint32_t o) { *(uint32_t *)(p + o) = swap32(*(uint32_t *)(p + o)); }

#define DESC_PORT 0
#define DESC_OOL 1
#define DESC_OOL_PORTS 2
#define DESC_OOL_VOLATILE 3

/* Swap header and descriptors. to_host: the data is in guest byte order and goes to host byte order. */
/* value of a word in host byte order, whether it is currently in guest or host byte order */
static inline uint32_t val(const uint8_t *p, uint32_t o, int guest) { return guest ? swap32(w_read(p, o)) : w_read(p, o); }

static void msg_swap(uint8_t *m, uint32_t size, int to_host)
{
    uint32_t bits = val(m, 0, to_host), n, i, o;
    for (o = 0; o < 24; o += 4) w_swap(m, o);
    if (!(bits & MACH_MSGH_BITS_COMPLEX) || size < 28) return;
    n = val(m, 24, to_host);
    w_swap(m, 24);
    /* 32 bit: port {name, pad, bit fields}, OOL {address, size, bit fields}, OOL ports {address, count,
       bit fields}; the type is always in the last byte */
    for (i = 0, o = 28; i < n && o + 12 <= size; i++, o += 12) {
        uint8_t type = m[o + 11];
        uint32_t addr = val(m, o, to_host), count = val(m, o + 4, to_host);
        w_swap(m, o);
        w_swap(m, o + 4);
        if (type == DESC_OOL_PORTS && addr) {
            /* list of port names: the kernel reads or writes it in host byte order */
            uint32_t *plist = (uint32_t *)(uintptr_t)addr, k;
            for (k = 0; k < count; k++) plist[k] = swap32(plist[k]);
        }
    }
}

/* ---------- font server (ATS) ----------
 * A separate Mach protocol without MIG: FontMgrMessage, 1740 bytes, msgh_id = message type (reply: status), content
 * from 24 raw in the byte order of the sender. The system's ATSServer (PowerPC) swaps nothing for Intel clients (the
 * place for it in the sync block is empty in its version), the Intel server is encrypted (DSMOS) and does not run.
 * Therefore rosetta swaps the numeric fields per message type according to a table (read off Intel's client and the
 * PowerPC server); strings and bytes stay raw. The client reads the shared database (OOL memory, big endian) itself
 * with swapping as soon as the rendezvous reports that the server has the other byte order (byte 0x40 of the reply).
 * rosetta remembers the port of the service from the look_up reply; ROSETTA_ATS_LOG=1 shows every message. */
#define ATS_SERVICE "com.apple.FontObjectsServer"
#define BS_LOOK_UP 404
#define ATS_SIZE 0x6cc
static mach_port_t g_ats_port;              /* send right to the font server (0: unknown) */
static int g_ats_log = -1;

enum { F_END, F_W, F_H, F_D, F_COUNT };
typedef struct { uint8_t kind; uint16_t n, o; } AtsField;
#define D(o) {F_D, 1, o}                    /* 64 bit (addresses in the server: mach_vm_address_t) */
#define W(o) {F_W, 1, o}                    /* 32 bit */
#define WN(o, n) {F_W, n, o}                /* n words in a row */
#define H(o) {F_H, 1, o}                    /* 16 bit */
#define HN(o, n) {F_H, n, o}                /* n 16-bit values in a row */
#define COUNT(o, n) {F_COUNT, n, o}         /* word at o: count (at most n) of the words from o + 4 */
#define LIST_END {F_END, 0, 0}
typedef struct {
    const char *name;
    AtsField request[12], reply[8];                 /* request; reply without sync block */
    uint8_t unchecked;                     /* contains ranges whose layout nobody has examined yet */
    uint8_t ofa;                            /* OFA message: attribute group, see ofa_content */
} AtsType;

/* Offsets from the start of the message. Fields below the content of a complex message (descriptors) are already
   swapped by msg_swap and are skipped here. */
static const AtsType g_ats_types[0x2c] = {
    [0x00] = { "Rendezvous", { W(0x34), H(0x38), W(0x3c), LIST_END }, { W(0x34), H(0x38), W(0x3c), LIST_END }, 0 },
    [0x01] = { "Sync", { LIST_END }, { LIST_END }, 0 },
    /* font at 0x18, then a count and as many table tags; reply: sync block */
    [0x02] = { "SynthesizeTables", { W(0x18), COUNT(0x1c, 32), LIST_END }, { LIST_END }, 0 },
    /* request: u16 at 0x18, options at 0x1c, path from 0x20 (0x1e: a second string from 0x220); reply: sync block,
       container at 0x124 (read off _SendActivateFontsMessage) */
    [0x03] = { "ActivateFonts", { H(0x18), W(0x1c), LIST_END }, { W(0x124), LIST_END }, 0 },
    [0x05] = { "ActivateFontsFromMemory", { H(0x28), W(0x2c), W(0x30), LIST_END }, { W(0x124), LIST_END }, 0 },
    [0x06] = { "DeactivateFontsInContainer", { H(0x18), W(0x1c), W(0x20), LIST_END }, { LIST_END }, 0 },
    [0x07] = { "GetContainerMapping", { W(0x18), LIST_END }, { W(0x1c), W(0x20), LIST_END }, 0 },
    [0x08] = { "GetAnnexData", { W(0x18), W(0x1c), LIST_END }, { W(0x1c), W(0x20), W(0x28), LIST_END }, 0 },
    /* reply: a flattened FSRef from 0x18; ats_reply turns it into the path of the file (ats_fsref_path) */
    [0x09] = { "GetFSRefForFileToken", { H(0x18), LIST_END }, { LIST_END }, 0 },
    [0x0a] = { "ResolveFileToken", { H(0x18), H(0x1a), LIST_END }, { H(0x18), LIST_END }, 0 },   /* path from 0x1c */
    [0x0b] = { "ComputeFontSpecs", { W(0x18), LIST_END }, { LIST_END }, 0 },
    [0x0c] = { "MarkFontAsBad", { W(0x18), W(0x1c), LIST_END }, { LIST_END }, 0 },
    [0x0d] = { "Notify", { LIST_END }, { LIST_END }, 0 },
    [0x0e] = { "ScanFontDirectories", { H(0x18), H(0x1a), LIST_END }, { W(0x124), LIST_END }, 0 },
    [0x10] = { "Notify", { LIST_END }, { LIST_END }, 0 },
    [0x12] = { "AddToFontNamesCache", { LIST_END }, { LIST_END }, 1 },
    [0x13] = { "FindUnicodeEncoding", { W(0x18), LIST_END }, { LIST_END }, 0 },
    /* reply: see fcache_reply */
    [0x14] = { "GetFCacheData", { H(0x18), H(0x1a), W(0x1c), W(0x20), LIST_END }, { W(0x18), LIST_END }, 0 },
    [0x15] = { "MapSharedMemory", { W(0x34), D(0x38), W(0x40), LIST_END }, { D(0x38), LIST_END }, 0 },
    [0x16] = { "FindFontIDFromName", { H(0x18), COUNT(0x1c, 20), WN(0x70, 5), LIST_END }, { WN(0x124, 6), LIST_END }, 0 },
    [0x17] = { "StandardFontDirectories", { LIST_END }, { W(0x1c), W(0x20), LIST_END }, 0 },
    [0x18] = { "QueryPort", { LIST_END }, { LIST_END }, 1 },
    [0x1a] = { "SynthesizeFontFamilyResources", { H(0x18), W(0x1c), LIST_END }, { W(0x1c), W(0x20), LIST_END }, 0 },
    /* reply: a byte at 0x18, the 256 16-bit entries of the encoding from 0x1a (ATSFontGetEncoding) */
    [0x1b] = { "GetPSFontEncoding", { W(0x18), LIST_END }, { HN(0x1a, 256), LIST_END }, 0 },
    [0x1c] = { "EnableFont", { W(0x1c), W(0x20), LIST_END }, { LIST_END }, 0 },
    [0x1d] = { "FODBDump", { H(0x18), LIST_END }, { LIST_END }, 0 },
    [0x1e] = { "ActivateFonts", { H(0x18), W(0x1c), LIST_END }, { W(0x124), LIST_END }, 0 },
    [0x1f] = { "OFAStream", { LIST_END }, { LIST_END }, 0, 1 },
    [0x20] = { "OFAStrike", { LIST_END }, { LIST_END }, 0, 1 },
    [0x21] = { "OFAGeneral", { LIST_END }, { LIST_END }, 0, 1 },
    [0x25] = { "Notify", { LIST_END }, { LIST_END }, 0 },
    [0x26] = { "Notify", { LIST_END }, { LIST_END }, 0 },
    [0x27] = { "GetPersistentData", { H(0x18), H(0x1a), LIST_END }, { W(0x18), W(0x1c), W(0x20), LIST_END }, 0 },
    [0x28] = { "SavePersistentData", { W(0x28), LIST_END }, { LIST_END }, 0 },
    [0x29] = { "GetFontProtectionInfo", { W(0x18), LIST_END }, { W(0x18), W(0x1c), LIST_END }, 0 },
    [0x2a] = { "GetFontTraits", { W(0x18), LIST_END }, { WN(0x18, 4), LIST_END }, 0 },
    [0x2b] = { "SetFontFlags", { WN(0x18, 3), LIST_END }, { LIST_END }, 0 },
};
static uint8_t g_ats_reported[0x2c + 1];

/* Sync block (DBSynchMessageReply from 0x18): 8 OOL descriptors, then 32-bit numbers (some unaligned) with three
   exceptions: 0x80 is the 64-bit address of the database in the server (the client computes gBaseAddressDelta from
   it), 0xa8 a 16-bit number, 0x116 a RelativePtr<ScalerList> that the client swaps itself (ERSSwap4). Identified by:
   complex with exactly 8 descriptors. */
#define SYNC_END 0x122

static inline void h_swap(uint8_t *p, uint32_t o) { uint8_t t = p[o]; p[o] = p[o + 1]; p[o + 1] = t; }
static inline void w_swap_u(uint8_t *p, uint32_t o)  /* unaligned */
{
    uint8_t t = p[o]; p[o] = p[o + 3]; p[o + 3] = t;
    t = p[o + 1]; p[o + 1] = p[o + 2]; p[o + 2] = t;
}
static inline void d_swap(uint8_t *p, uint32_t o)
{
    uint32_t i;
    for (i = 0; i < 4; i++) { uint8_t t = p[o + i]; p[o + i] = p[o + 7 - i]; p[o + 7 - i] = t; }
}

static uint32_t field_end(const AtsField *f)
{
    return f->o + (f->kind == F_H ? 2 * f->n : f->kind == F_D ? 8 * f->n : f->kind == F_COUNT ? 4 + 4 * f->n : 4 * f->n);
}

/* Swap fields; start: start of content, end: message size. to_host: the fields are in guest byte order.
   skip: leave out fields that overlap one of these (already swapped) */
static void ats_fields(uint8_t *m, uint32_t start, uint32_t end, const AtsField *f, int to_host, const AtsField *skip)
{
    for (; f->kind != F_END; f++) {
        uint32_t e = field_end(f), k;
        const AtsField *g;
        if (f->o < start) continue;
        for (g = skip; g && g->kind != F_END; g++) if (f->o < field_end(g) && g->o < e) break;
        if (g && g->kind != F_END) continue;
        if (f->kind == F_H) { for (k = 0; k < f->n && f->o + 2 * k + 2 <= end; k++) h_swap(m, f->o + 2 * k); continue; }
        if (f->kind == F_D) { for (k = 0; k < f->n && f->o + 8 * k + 8 <= end; k++) d_swap(m, f->o + 8 * k); continue; }
        if (f->kind == F_W) { for (k = 0; k < f->n && f->o + 4 * k + 4 <= end; k++) w_swap_u(m, f->o + 4 * k); continue; }
        if (f->o + 4 > end) continue;
        {
            uint32_t count = *(uint32_t *)(m + f->o);
            if (to_host) count = swap32(count);
            if (count > f->n) count = f->n;
            w_swap_u(m, f->o);
            for (k = 0; k < count && f->o + 8 + 4 * k <= end; k++) w_swap_u(m, f->o + 4 + 4 * k);
        }
    }
}

/* start of content of a message whose header is in host byte order */
static uint32_t content_start(const uint8_t *m, uint32_t n)
{
    const mach_msg_header_t *k = (const mach_msg_header_t *)m;
    if (!(k->msgh_bits & MACH_MSGH_BITS_COMPLEX) || n < 28) return 24;
    return 28 + 12 * *(const uint32_t *)(m + 24);
}

static void ats_report(uint32_t type)
{
    const AtsType *t = type < 0x2c ? &g_ats_types[type] : 0;
    uint32_t i = type < 0x2c ? type : 0x2c;
    if (g_ats_reported[i]) return;
    g_ats_reported[i] = 1;
    if (!t || !t->name) fprintf(stderr, "rosetta: ATS message type 0x%x without layout, content stays raw\n", type);
    else if (t->unchecked) fprintf(stderr, "rosetta: ATS message type 0x%x (%s) only partly converted\n", type, t->name);
}

/* ---- OFA (glyph strikes): content after the descriptors {u8 a, b, c, embedded; u32 size; u32 offset;
   u32 server generation; u8 e; data from +0x11}. The data (embedded, OOL or in shared memory) are mostly attribute
   groups: header {type, length in bytes, check value, count}, attributes {tag, size, data}; size: lower 30 bits are
   words including the attribute header, upper 2 bits are padding bytes in the last word. Values are almost always
   32-bit words or nested groups; exceptions per tag: 'gary' (glyph numbers, 16 bit), 'dest' (target: two 64-bit
   addresses of the shared memory, in the server and in the client). Everything else (C structures such as the
   strike metrics) word by word. */
#define K4(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
static uint32_t read_u(const uint8_t *p, int to_host)  /* value of a word that is in source byte order */
{
    uint32_t w = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    return to_host ? swap32(w) : w;
}

static void words_swap(uint8_t *p, uint32_t len)
{
    uint32_t o;
    for (o = 0; o + 4 <= len; o += 4) w_swap_u(p, o);
}

/* whether the range looks like an attribute group (length in header = range, attributes fill it exactly) */
static int ag_group(const uint8_t *p, uint32_t len, int to_host)
{
    uint32_t cnt, o = 16, i;
    if (len < 16 || read_u(p + 4, to_host) != len) return 0;
    cnt = read_u(p + 12, to_host);
    if (cnt > 1024) return 0;
    for (i = 0; i < cnt; i++) {
        uint32_t w;
        if (o + 8 > len) return 0;
        w = read_u(p + o + 4, to_host) & 0x3fffffff;
        if (w < 2 || o + 4 * w > len) return 0;
        o += 4 * w;
    }
    return o == len;
}

/* copy of an OOL range of the request (the guest memory stays in its byte order); target ('dest') of the request */
typedef struct {
    vm_address_t copy, original; vm_size_t size; int original_release;
    uint32_t target, target_len;             /* result in shared memory: client address, length */
    uint32_t mtyp, rtyp, cfmt, glyphs;     /* requested metric, result kind and format, number of glyphs ('gary') */
} AtsCopy;

static void ag_swap(uint8_t *p, uint32_t len, int to_host, int depth, AtsCopy *ko)
{
    uint32_t cnt, o = 16, i;
    if (depth > 8 || !ag_group(p, len, to_host)) { words_swap(p, len); return; }
    cnt = read_u(p + 12, to_host);
    words_swap(p, 16);
    for (i = 0; i < cnt; i++) {
        uint8_t *a = p + o, *d = a + 8;
        uint32_t tag = read_u(a, to_host), g = read_u(a + 4, to_host), w = g & 0x3fffffff, k;
        uint32_t gross = 4 * w - (g >> 30), dl = gross > 8 ? gross - 8 : 0;
        words_swap(a, 8);
        if (ko && to_host && dl >= 4 && (tag == K4('m', 't', 'y', 'p') || tag == K4('r', 't', 'y', 'p') ||
                                         tag == K4('c', 'f', 'm', 't')))
            *(tag == K4('m', 't', 'y', 'p') ? &ko->mtyp : tag == K4('r', 't', 'y', 'p') ? &ko->rtyp : &ko->cfmt) = read_u(d, 1);
        if (tag == K4('g', 'a', 'r', 'y')) {
            if (ko && to_host) ko->glyphs = dl / 2;
            for (k = 0; k + 2 <= dl; k += 2) h_swap(d, k);
        } else if (tag == K4('d', 'e', 's', 't') && dl >= 0x18) {
            if (ko && to_host && read_u(d + 0x14, 1) == K4('s', 'h', 'm', 'o')) {
                ko->target_len = read_u(d, 1);
                ko->target = read_u(d + 0xc, 1);
            }
            w_swap_u(d, 0); d_swap(d, 4); d_swap(d, 0xc);
            words_swap(d + 0x14, dl - 0x14);
        } else if (ag_group(d, dl, to_host)) {
            ag_swap(d, dl, to_host, depth + 1, ko);
        } else words_swap(d, dl);
        o += 4 * w;
    }
}

/* The count word at 0x18 is always present (also without descriptors: then 0), the content follows the descriptors.
   Without the COMPLEX bit msg_swap has not touched the word: swap it here as well. */
static void ofa_content(uint8_t *m, uint32_t n, int to_host)
{
    int is_complex = (((mach_msg_header_t *)m)->msgh_bits & MACH_MSGH_BITS_COMPLEX) != 0;
    uint32_t cnt, b, sz;
    if (n < 0x1c) return;
    cnt = *(uint32_t *)(m + 24);
    if (!is_complex && to_host) cnt = swap32(cnt);
    if (!is_complex) w_swap(m, 24);
    if (cnt > 8) return;
    b = 0x1c + 12 * cnt;
    if (b + 0x11 > n) return;
    sz = *(uint32_t *)(m + b + 4);
    if (to_host) sz = swap32(sz);
    w_swap_u(m, b + 4); w_swap_u(m, b + 8); w_swap_u(m, b + 0xc);
    if (m[b + 3] && sz <= n - (b + 0x11)) ag_swap(m + b + 0x11, sz, to_host, 0, 0);   /* embedded */
}

/* ROSETTA_ATS_LOG=3: snapshot of the shared memory around the target before the request, changed ranges after it */
static uint8_t *g_snap;
static vm_address_t g_snap_addr;
static vm_size_t g_snap_size;

static void shm_snapshot(uint32_t target)
{
    vm_address_t a = target;
    vm_size_t sz = 0;
    vm_region_basic_info_data_t info;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT;
    mach_port_t obj;
    free(g_snap); g_snap = 0;
    if (vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS)
        return;
    if (a > target || sz > 0x400000 || !(g_snap = malloc(sz))) return;
    memcpy(g_snap, (const void *)a, sz);
    g_snap_addr = a; g_snap_size = sz;
}

static void shm_diff(void)
{
    uint32_t o, start = 0, in = 0;
    if (!g_snap) return;
    fprintf(stderr, "  [shm] region %08x+%x changed:", (uint32_t)g_snap_addr, (uint32_t)g_snap_size);
    for (o = 0; o + 4 <= g_snap_size; o += 4) {
        int ch = memcmp(g_snap + o, (const uint8_t *)g_snap_addr + o, 4) != 0;
        if (ch && !in) { start = o; in = 1; }
        if (!ch && in) { fprintf(stderr, " %x-%x", start, o); in = 0; }
    }
    if (in) fprintf(stderr, " %x-%x", start, o);
    fprintf(stderr, "\n");
    free(g_snap); g_snap = 0;
}

static void ofa_request(uint8_t *m, uint32_t n, AtsCopy *k)
{
    ofa_content(m, n, 1);
    if ((((mach_msg_header_t *)m)->msgh_bits & MACH_MSGH_BITS_COMPLEX) && *(uint32_t *)(m + 24) >= 1 && n >= 40) {
        uint8_t *e = m + 28;
        uint32_t addr = *(uint32_t *)e, sz = *(uint32_t *)(e + 4);
        vm_address_t fresh = 0;
        if ((e[11] != DESC_OOL && e[11] != DESC_OOL_VOLATILE) || !addr || !sz || sz > 0x1000000) return;
        if (vm_allocate(mach_task_self(), &fresh, sz, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) return;
        memcpy((void *)fresh, (const void *)(uintptr_t)addr, sz);
        ag_swap((uint8_t *)fresh, sz, 1, 0, k);
        *(uint32_t *)e = (uint32_t)fresh;
        k->copy = fresh; k->original = addr; k->size = sz;
        k->original_release = e[8];        /* deallocate: the kernel frees the copy, rosetta the original */
    }
    if (g_ats_log > 2 && k->target) shm_snapshot(k->target);
}

/* after mach_msg: free the copy or (with deallocate and a successful send) the original */
static void ofa_cleanup(AtsCopy *k, mach_msg_return_t r)
{
    int sent = r == MACH_MSG_SUCCESS || (r & 0xffffc000) == 0x10004000;   /* MACH_RCV_*: it has been sent */
    if (!k->copy) return;
    if (k->original_release && sent) vm_deallocate(mach_task_self(), k->original, k->size);
    else vm_deallocate(mach_task_self(), k->copy, k->size);
}

static void ofa_reply(uint8_t *m, uint32_t n, AtsCopy *k)
{
    ofa_content(m, n, 0);
    if ((((mach_msg_header_t *)m)->msgh_bits & MACH_MSGH_BITS_COMPLEX) && n >= 28) {
        uint32_t d, cnt = *(uint32_t *)(m + 24);
        for (d = 0; d < cnt && 28 + 12 * d + 12 <= n; d++) {
            uint8_t *e = m + 28 + 12 * d;
            uint32_t addr = *(uint32_t *)e, sz = *(uint32_t *)(e + 4);
            if ((e[11] == DESC_OOL || e[11] == DESC_OOL_VOLATILE) && addr && sz <= 0x1000000)
                ag_swap((uint8_t *)(uintptr_t)addr, sz, 0, 0, 0);   /* freshly mapped, belongs to us */
        }
    }
    /* result in the client's shared memory ('dest' = 'shmo'): the server has written it there in its byte order.
       Metrics per glyph ('mtyp'): 'mfrc' 24 bytes of Fixed values; 'mrdr' 36 bytes {Fixed advance x, y,
       origin x, y; u16 height, width; Fixed left bearing, top, right bearing, 0}. Glyph paths ('rtyp' = 'path',
       format 'gxpt': QuickDraw GX paths, contour count, then per contour point count, control bit words, Fixed
       points): 32-bit values. Bitmaps of a bitmap strike ('rtyp' = 'bmap', format 'cfmt' = 'mtbm', no 'mtyp'):
       per glyph the 36 bytes of 'mrdr' followed by the image, height rows of (width + 7) / 8 bytes (one bit per
       pixel), padded to an even length; the next glyph follows directly (2-byte aligned). The image bytes stay as
       they are. */
    if (k->target && k->target_len && k->target_len <= 0x100000 && ((mach_msg_header_t *)m)->msgh_id == 0) {
        uint8_t *p = (uint8_t *)(uintptr_t)k->target;
        uint32_t o = 0, i;
        static uint32_t reported[4];
        if (g_ats_log > 2) shm_diff();
        if (g_ats_log > 0) {
            fprintf(stderr, "  [target] %08x+%x:", k->target, k->target_len);
            for (i = 0; i + 4 <= k->target_len && i < (g_ats_log > 1 ? 4096u : 256u); i += 4) fprintf(stderr, " %08x", *(uint32_t *)(uintptr_t)(k->target + i));
            fprintf(stderr, "\n");
        }
        int images = k->rtyp == K4('b', 'm', 'a', 'p') && k->cfmt == K4('m', 't', 'b', 'm') && !k->mtyp;
        if (k->mtyp == K4('m', 'r', 'd', 'r') || images)
            for (i = 0; i < k->glyphs && o + 0x24 <= k->target_len; i++) {
                uint8_t *r = p + o;
                uint32_t height = r[0x10] << 8 | r[0x11], width = r[0x12] << 8 | r[0x13];
                uint32_t n = images ? height * ((width + 7) / 8) : 0;      /* image bytes behind the header */
                words_swap(r, 0x10);
                h_swap(r, 0x10); h_swap(r, 0x12);
                words_swap(r + 0x14, 0x10);
                o += 0x24 + n + (n & 1);
            }
        for (i = 0; i < 3; i++) {
            uint32_t kind = i == 0 ? k->mtyp : i == 1 ? k->rtyp : k->cfmt;
            if (!kind || kind == K4('m', 'r', 'd', 'r') || kind == K4('m', 'f', 'r', 'c') || kind == K4('p', 'a', 't', 'h') ||
                kind == K4('m', 't', 'b', 'm') || kind == K4('g', 'x', 'p', 't') ||
                (kind == K4('b', 'm', 'a', 'p') && (images || k->mtyp == K4('m', 'r', 'd', 'r'))))
                continue;
            if (reported[0] != kind && reported[1] != kind && reported[2] != kind && reported[3] != kind) {
                fprintf(stderr, "rosetta: ATS result kind '%c%c%c%c' only converted word by word\n",
                        kind >> 24, (kind >> 16) & 0xff, (kind >> 8) & 0xff, kind & 0xff);
                reported[kind % 4] = kind;
            }
        }
        if (!images && o < k->target_len) ag_swap(p + o, k->target_len - o, 0, 0, 0);
    }
}

/* request (header already in host byte order, content in guest byte order) to server byte order */
static void ats_request(uint8_t *m, uint32_t n, AtsCopy *k)
{
    uint32_t type = ((mach_msg_header_t *)m)->msgh_id;
    if (type < 0x2c && g_ats_types[type].ofa) ofa_request(m, n, k);
    else if (type < 0x2c && g_ats_types[type].name) ats_fields(m, content_start(m, n), n, g_ats_types[type].request, 1, 0);
    if (type >= 0x2c || !g_ats_types[type].name || g_ats_types[type].unchecked) ats_report(type);
    if (n >= ATS_SIZE) { w_swap(m, 0x6c0); w_swap(m, 0x6c4); }   /* client ID, server generation */
}

/* GetFSRefForFileToken: the server sends a flattened FSRef. A client that the rendezvous told about the other byte
   order expects the path of the file instead (FSPathMakeRef in GetFSRefForFileToken of the Intel ATS), so the host's
   File Manager turns the FSRef into a path. CarbonCore is loaded only when a program asks for this. */
static int (*g_unflatten_fsref)(const void *flat, void *ref, void *unused);
static int (*g_fsref_path)(const void *ref, uint8_t *path, uint32_t size);
static pthread_once_t g_carbon_once = PTHREAD_ONCE_INIT;

static void carbon_load(void)
{
    void *h = dlopen("/System/Library/Frameworks/CoreServices.framework/Frameworks/CarbonCore.framework/CarbonCore",
                     RTLD_LAZY | RTLD_LOCAL);
    if (h) {
        g_unflatten_fsref = (int (*)(const void *, void *, void *))dlsym(h, "_FSUnflattenFSRef");
        g_fsref_path = (int (*)(const void *, uint8_t *, uint32_t))dlsym(h, "FSRefMakePath");
    }
    if (!g_unflatten_fsref || !g_fsref_path) fprintf(stderr, "rosetta: CarbonCore not usable, font files have no path\n");
}

static void ats_fsref_path(uint8_t *m, uint32_t n)
{
    uint8_t ref[80], path[1024];
    uint32_t room = (n < ATS_SIZE ? n : 0x6c0) - 0x18, len;
    if (n < 0x18 + 8 + sizeof ref) return;
    pthread_once(&g_carbon_once, carbon_load);
    if (!g_unflatten_fsref || !g_fsref_path || g_unflatten_fsref(m + 0x18, ref, 0) ||
        g_fsref_path(ref, path, sizeof path) || (len = strlen((const char *)path)) >= room) {
        m[0x18] = 0;
        return;
    }
    memcpy(m + 0x18, path, len + 1);
}

/* GetFCacheData (0x14) reply: complex, one OOL region per cache file; _SendGetFCacheDataMessage reads the descriptor
   count at 0x18 and address and size of each descriptor (msg_swap has converted them), then one word behind the
   descriptors and one 16-bit value per region. */
static void fcache_reply(uint8_t *m, uint32_t n)
{
    uint32_t cnt, o, i, end = n < ATS_SIZE ? n : 0x6c0;
    if (!(((mach_msg_header_t *)m)->msgh_bits & MACH_MSGH_BITS_COMPLEX)) {   /* no regions: only the count and the word */
        if (*(uint32_t *)(m + 0x18) == 0) w_swap(m, 0x1c);
        return;
    }
    cnt = *(uint32_t *)(m + 0x18);
    o = 0x1c + 12 * cnt;
    if (cnt > 32 || o + 4 + 2 * cnt > end) return;
    w_swap_u(m, o);
    for (o += 4, i = 0; i < cnt; i++, o += 2) h_swap(m, o);
}

/* reply (header already in host byte order, content in server byte order) to guest byte order. typ: type of the
   request. The server replies in the buffer of the request; what it does not overwrite is still in its byte order
   and is swapped back as well. */
static void ats_reply(uint8_t *m, uint32_t n, uint32_t type, AtsCopy *k)
{
    const AtsType *t = type < 0x2c && g_ats_types[type].name ? &g_ats_types[type] : 0;
    uint32_t start = content_start(m, n), o;
    int is_sync = (((mach_msg_header_t *)m)->msgh_bits & MACH_MSGH_BITS_COMPLEX) && n >= SYNC_END &&
                   *(uint32_t *)(m + 24) == 8;
    if (is_sync) {
        w_swap_u(m, 0x7c);
        d_swap(m, 0x80);
        for (o = 0x88; o < 0xa8; o += 4) w_swap_u(m, o);
        h_swap(m, 0xa8);
        for (o = 0xaa; o < 0x116; o += 4) w_swap_u(m, o);
        w_swap_u(m, 0x11a); w_swap_u(m, 0x11e);
        start = SYNC_END;
    }
    if (t && t->ofa && !is_sync) ofa_reply(m, n, k);
    else if (type == 9 && !is_sync && ((mach_msg_header_t *)m)->msgh_id == 0) ats_fsref_path(m, n);
    else if (type == 0x14 && !is_sync && ((mach_msg_header_t *)m)->msgh_id == 0 && n >= 0x20) fcache_reply(m, n);
    else if (t) {
        ats_fields(m, start, n, t->reply, 0, 0);
        ats_fields(m, start, n, t->request, 0, t->reply);
    }
    if (type == 0 && n > 0x40) m[0x40] = 0;  /* rendezvous: the server has the other byte order */
    if (n >= ATS_SIZE) { w_swap(m, 0x6c0); w_swap(m, 0x6c4); }
}

static void ats_show(const char *was, const uint8_t *m, uint32_t n, int guest)
{
    uint32_t i;
    if (g_ats_log < 0) g_ats_log = getenv("ROSETTA_ATS_LOG") ? atoi(getenv("ROSETTA_ATS_LOG")) : 0;
    if (!g_ats_log) return;
    fprintf(stderr, "[ats %s] id %d size %u:", was, (int32_t)(guest ? swap32(*(const uint32_t *)(m + 20)) : *(const uint32_t *)(m + 20)), n);
    for (i = 24; i + 4 <= n && i < 24 + 4 * 48; i += 4) fprintf(stderr, " %08x", *(const uint32_t *)(m + i));
    if (n >= ATS_SIZE) fprintf(stderr, " | 6c0: %08x %08x %02x", *(const uint32_t *)(m + 0x6c0), *(const uint32_t *)(m + 0x6c4), m[0x6c8]);
    fprintf(stderr, "\n");
    /* OOL contents (header in host byte order): the first 256 bytes per descriptor */
    if ((((const mach_msg_header_t *)m)->msgh_bits & MACH_MSGH_BITS_COMPLEX) && n >= 28 && !guest) {
        uint32_t d, cnt = *(const uint32_t *)(m + 24);
        for (d = 0; d < cnt && d < 8 && 28 + 12 * d + 12 <= n; d++) {
            const uint8_t *e = m + 28 + 12 * d;
            uint32_t addr = *(const uint32_t *)e, sz = *(const uint32_t *)(e + 4), k;
            if (e[11] != DESC_OOL && e[11] != DESC_OOL_VOLATILE) continue;
            if (addr == 0 || sz > 0x100000) continue;
            fprintf(stderr, "  [ool %u] %08x+%x:", d, addr, sz);
            for (k = 0; k + 4 <= sz && k < (g_ats_log > 1 ? 4096u : 256u); k += 4) fprintf(stderr, " %08x", *(const uint32_t *)(uintptr_t)(addr + k));
            fprintf(stderr, "\n");
        }
    }
}

/* host_info(HOST_BASIC_INFO) (NXGetLocalArchInfo, arch, machine): the guest sees the Intel processor that CPUID
   reports (see sys.c, g_identity). Reply in host byte order: RetCode @32, count @36, host_basic_info from @40 with
   cpu_type @52, cpu_subtype @56. */
#define HOST_INFO_ID 200
extern int g_host_cpu;

static void host_info_reply(uint8_t *m, uint32_t n)
{
    if (g_host_cpu < 0) g_host_cpu = getenv("ROSETTA_HOST_CPU") != 0;
    if (g_host_cpu || n < 60 || ((mach_msg_header_t *)m)->msgh_id != HOST_INFO_ID + 100) return;
    if (*(uint32_t *)(m + 32) != KERN_SUCCESS || *(uint32_t *)(m + 36) < 5) return;
    *(uint32_t *)(m + 52) = CPU_TYPE_X86;
    *(uint32_t *)(m + 56) = 4;              /* CPU_SUBTYPE_486, as Intel Macs report it ("machine": i486) */
}

/* io_registry_entry_from_path("IODeviceTree:/efi"): Intel frameworks treat a missing EFI node as a fatal platform
   error. AppKit, when the program starts its first NSThread ([NSApplication _taskNowMultiThreaded:]), then waits
   forever on a condition nobody signals; LaunchServices waits for the node to appear. A PowerPC Mac has Open Firmware
   instead of EFI: the query goes to the device tree root, which exists. Request (MIG, iokit 2809): NDR @24,
   pathOffset @32, pathCnt @36 (guest byte order, with the NUL), path from @40; size 40 + pathCnt rounded to 4. */
#define IO_REGISTRY_ENTRY_FROM_PATH 2809
extern volatile int g_checkpoint_idle;      /* jit.c: write the translation cache at the next dispatcher round */
static const char g_efi_path[] = "IODeviceTree:/efi", g_dt_root[] = "IODeviceTree:/";

static uint32_t efi_query(uint8_t *m, uint32_t size)
{
    mach_msg_header_t *h = (mach_msg_header_t *)m;
    uint32_t cnt;
    if (size < 40 + sizeof g_efi_path || h->msgh_id != IO_REGISTRY_ENTRY_FROM_PATH) return size;
    cnt = swap32(*(uint32_t *)(m + 36));
    if (cnt != sizeof g_efi_path || memcmp(m + 40, g_efi_path, sizeof g_efi_path)) return size;
    memset(m + 40, 0, (sizeof g_efi_path + 3) & ~3u);
    memcpy(m + 40, g_dt_root, sizeof g_dt_root);
    *(uint32_t *)(m + 36) = swap32((uint32_t)sizeof g_dt_root);
    h->msgh_size = 40 + (((uint32_t)sizeof g_dt_root + 3) & ~3u);
    if (g_log) fprintf(stderr, "[iokit] IODeviceTree:/efi answered with the device tree root\n");
    return h->msgh_size;
}

/* thread_get_state (MIG 3603) for a guest thread: the kernel knows only the PowerPC state of the host thread (flavor
   1 is PPC_THREAD_STATE there, 40 words), so a garbage collector that suspends the other threads and scans their
   registers and stacks (libauto: Objective-C 2.0 garbage collection in Xcode, Property List Editor ...) stopped with
   a fatal error. Answered here from the thread's Cpu: a thread that does not hold the lock waits where its registers
   are stored. Flavors x86_THREAD_STATE32 (1, 16 words) and x86_THREAD_STATE (7, with flavor and count first).
   Request (guest order body): NDR @24, flavor @32, count @36. Reply (header in host order, body in guest order):
   NDR, RetCode @32, count @36, state from @40; id 3703. Returns the size of the reply in g_buffer, 0 if not ours. */
#define THREAD_GET_STATE 3603
static uint32_t thread_state_reply(uint8_t *m, uint32_t size)
{
    mach_msg_header_t *h = (mach_msg_header_t *)m;
    uint32_t flavor, cnt, w[18], n, i, k = 0;
    Cpu *t;
    if (size < 40 || h->msgh_id != THREAD_GET_STATE || !(t = thread_by_port(h->msgh_remote_port))) return 0;
    flavor = swap32(*(uint32_t *)(m + 32));
    cnt = swap32(*(uint32_t *)(m + 36));
    if (flavor != 1 && flavor != 7) return 0;           /* other flavors: the kernel (and its answer) as before */
    n = flavor == 1 ? 16 : 18;
    if (flavor == 7) { w[k++] = 1; w[k++] = 16; }
    w[k++] = t->r[EAX]; w[k++] = t->r[EBX]; w[k++] = t->r[ECX]; w[k++] = t->r[EDX];
    w[k++] = t->r[EDI]; w[k++] = t->r[ESI]; w[k++] = t->r[EBP]; w[k++] = t->r[ESP];
    w[k++] = 0x1f;                                      /* ss */
    w[k++] = flags_fetch(t);
    w[k++] = t->eip;
    w[k++] = 0x17; w[k++] = 0x1f; w[k++] = 0x1f; w[k++] = 0x1f; w[k++] = 0x37;   /* cs ds es fs gs as on Intel Leopard */
    h->msgh_bits = MACH_MSGH_BITS(0, MACH_MSG_TYPE_PORT_SEND_ONCE);
    h->msgh_remote_port = MACH_PORT_NULL;
    h->msgh_id = THREAD_GET_STATE + 100;
    if (cnt < n) {                                      /* as the kernel: the buffer is too small */
        *(uint32_t *)(m + 32) = swap32(KERN_INVALID_ARGUMENT);
        *(uint32_t *)(m + 36) = 0;
        n = 0;
    } else {
        *(uint32_t *)(m + 32) = 0;
        *(uint32_t *)(m + 36) = swap32(n);
        for (i = 0; i < n; i++) *(uint32_t *)(m + 40 + 4 * i) = swap32(w[i]);
    }
    h->msgh_size = 40 + 4 * n;
    *(uint32_t *)(m + h->msgh_size) = MACH_MSG_TRAILER_FORMAT_0;   /* trailer: type and size, host order */
    *(uint32_t *)(m + h->msgh_size + 4) = sizeof(mach_msg_trailer_t);
    if (g_log) fprintf(stderr, "[mach] thread_get_state of guest thread %x answered (flavor %u)\n", t->mach_thread, flavor);
    return h->msgh_size;
}

/* ROSETTA_STATS: per message id sent (receive only: id 0) count, time in mach_msg (kernel and the server's answer)
   and time converting; shown at the first idle moment (mach_msg_stats) */
#define MSG_STATS 256
static struct { int id; unsigned n; uint64_t host, conv; } g_msg_st[MSG_STATS];
static int g_msg_measure = -1;

static void msg_stat(int id, uint64_t host, uint64_t conv)
{
    uint32_t h = ((uint32_t)id * 2654435761u) >> 24, k;
    for (k = 0; k < MSG_STATS; k++, h = (h + 1) & (MSG_STATS - 1))
        if (g_msg_st[h].id == id || !g_msg_st[h].n) {
            g_msg_st[h].id = id; g_msg_st[h].n++; g_msg_st[h].host += host; g_msg_st[h].conv += conv;
            return;
        }
}

void mach_msg_stats(void)
{
    mach_timebase_info_data_t tb;
    uint64_t host = 0, conv = 0;
    unsigned n = 0;
    int k, z, best;
    if (g_msg_measure <= 0) return;
    mach_timebase_info(&tb);
    for (k = 0; k < MSG_STATS; k++) { host += g_msg_st[k].host; conv += g_msg_st[k].conv; n += g_msg_st[k].n; }
    fprintf(stderr, "rosetta: %u Mach messages, %.1f ms in mach_msg, %.1f ms converting; by id (0: receive only):", n,
            (double)host * tb.numer / tb.denom / 1e6, (double)conv * tb.numer / tb.denom / 1e6);
    for (z = 0; z < 16; z++) {
        for (best = -1, k = 0; k < MSG_STATS; k++)
            if (g_msg_st[k].n && (best < 0 || g_msg_st[k].host > g_msg_st[best].host)) best = k;
        if (best < 0) break;
        fprintf(stderr, " %d:%ux/%.1f/%.1f", g_msg_st[best].id, g_msg_st[best].n,
                (double)g_msg_st[best].host * tb.numer / tb.denom / 1e6, (double)g_msg_st[best].conv * tb.numer / tb.denom / 1e6);
        g_msg_st[best].n = 0;
    }
    fprintf(stderr, " ms\n");
}

/* buffer per thread (Cpu.msg): during mach_msg the thread lock is free, another thread can send */
static mach_msg_return_t guest_mach_msg_call(Cpu *c, uint32_t msg, mach_msg_option_t option, uint32_t send_size, uint32_t receive,
                                            mach_port_t name, mach_msg_timeout_t deadline, mach_port_t message, uint64_t *host);

static mach_msg_return_t guest_mach_msg(Cpu *c, uint32_t msg, mach_msg_option_t option, uint32_t send_size, uint32_t receive,
                                       mach_port_t name, mach_msg_timeout_t deadline, mach_port_t message)
{
    uint64_t t0, host = 0;
    mach_msg_return_t r;
    int id;
    if (g_msg_measure < 0) g_msg_measure = getenv("ROSETTA_STATS") != 0;
    if (!g_msg_measure) return guest_mach_msg_call(c, msg, option, send_size, receive, name, deadline, message, 0);
    id = (option & MACH_SEND_MSG) && send_size >= 24 ? (int)ld32(msg + 20) : 0;
    t0 = mach_absolute_time();
    r = guest_mach_msg_call(c, msg, option, send_size, receive, name, deadline, message, &host);
    msg_stat(id, host, mach_absolute_time() - t0 - host);
    return r;
}

static mach_msg_return_t guest_mach_msg_call(Cpu *c, uint32_t msg, mach_msg_option_t option, uint32_t send_size, uint32_t receive,
                                            mach_port_t name, mach_msg_timeout_t deadline, mach_port_t message, uint64_t *host)
{
    size_t need = (send_size > receive ? send_size : receive) + 256;
    mach_msg_return_t r;
    uint8_t *g_buffer;
    size_t g_buffer_size;
    int ats_search = 0, ats_query = 0, basic_info = 0;
    uint32_t ats_type = 0, own = 0;
    AtsCopy ats_copy = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (need > c->msg_size) {
        free(c->msg);
        c->msg_size = need < 65536 ? 65536 : (uint32_t)need;
        c->msg = malloc(c->msg_size);
    }
    g_buffer = c->msg;
    g_buffer_size = c->msg_size;
    if (option & MACH_SEND_MSG) {
        memcpy(g_buffer, (const void *)(uintptr_t)msg, send_size);
        msg_swap(g_buffer, send_size, 1);
        if (g_ats_port && ((mach_msg_header_t *)g_buffer)->msgh_remote_port == g_ats_port && send_size >= 24) {
            ats_type = ((mach_msg_header_t *)g_buffer)->msgh_id;
            ats_request(g_buffer, send_size, &ats_copy);
            ats_show("to", g_buffer, send_size, 0);
            ats_query = 1;
        }
        ats_search = send_size >= 160 && ((mach_msg_header_t *)g_buffer)->msgh_id == BS_LOOK_UP &&
                    !strncmp((const char *)(g_buffer + 32), ATS_SERVICE, 128);
        basic_info = send_size >= 40 && ((mach_msg_header_t *)g_buffer)->msgh_id == HOST_INFO_ID &&
                     swap32(*(uint32_t *)(g_buffer + 32)) == HOST_BASIC_INFO;    /* flavor, still in guest order */
        send_size = efi_query(g_buffer, send_size);
        own = thread_state_reply(g_buffer, send_size);
    }
    if (own) { r = MACH_MSG_SUCCESS; goto received; }    /* answered here (thread_get_state of a guest thread) */
    {
        /* Wait interruptibly: otherwise the libSystem wrapper of the host silently restarts an interrupted send/receive,
           and a guest signal (SIGTERM to a Cocoa program in the event loop) would never arrive. Without a pending
           signal (profile clock, watchdog) retry here, after a completed send only receive */
        uint32_t saved_base = g_base;
        mach_msg_option_t o = option | MACH_SEND_INTERRUPT | MACH_RCV_INTERRUPT;
        /* the main thread waits for events without a deadline for the first time: startup is over, the translation
           cache gets a checkpoint while every guest thread waits (jit.c, thread.c) */
        static int idle_seen;
        Cpu *fc;
        if (!idle_seen && c == g_main_cpu && (option & MACH_RCV_MSG) && !(option & MACH_SEND_MSG) &&
            (!(option & MACH_RCV_TIMEOUT) || deadline >= 1000)) {
            idle_seen = 1;
            g_checkpoint_idle = 1;
            thread_watchdog_start();
        }
        uint64_t h0 = host ? mach_absolute_time() : 0;
        fc = glock_release();                    /* receiving can wait: let other threads run */
        for (;;) {
            r = mach_msg((mach_msg_header_t *)g_buffer, o, send_size, receive, name, deadline, message);
            if (r != MACH_SEND_INTERRUPTED && r != MACH_RCV_INTERRUPTED) break;
            if (option & (r == MACH_SEND_INTERRUPTED ? MACH_SEND_INTERRUPT : MACH_RCV_INTERRUPT)) break;
            if (g_sig_pending & ~g_sig_mask) break;
            if (r == MACH_RCV_INTERRUPTED) { o &= ~MACH_SEND_MSG; send_size = 0; }
        }
        if (host) *host = mach_absolute_time() - h0;   /* (with waiting for the lock below not included) */
        glock_acquire(fc);
        g_base = saved_base;
        ofa_cleanup(&ats_copy, r);
        /* guest signal pending: the guest wrapper retries at once; first stop all block entries so it enters the
           dispatcher on the way (delivers the signal, sigreturn leads back into the new attempt) */
        if ((r == MACH_SEND_INTERRUPTED || r == MACH_RCV_INTERRUPTED) && (g_sig_pending & ~g_sig_mask) &&
            !(option & (r == MACH_SEND_INTERRUPTED ? MACH_SEND_INTERRUPT : MACH_RCV_INTERRUPT)))
            jit_interrupt();
    }
received:
    if ((r == MACH_SEND_INTERRUPTED || r == MACH_RCV_INTERRUPTED) && getenv("ROSETTA_HPROFILE"))
        fprintf(stderr, "rosetta: mach_msg interrupted (%x)\n", r);
    if ((option & MACH_RCV_MSG) && (r == MACH_MSG_SUCCESS || r == MACH_RCV_TOO_LARGE)) {
        mach_msg_header_t *k = (mach_msg_header_t *)g_buffer;
        uint32_t n = k->msgh_size, t = 0;
        if (r == MACH_RCV_TOO_LARGE) {
            msg_swap(g_buffer, sizeof *k, 0);
            memcpy((void *)(uintptr_t)msg, g_buffer, sizeof *k);
            return r;
        }
        if (ats_search && n >= 40 && (k->msgh_bits & MACH_MSGH_BITS_COMPLEX)) g_ats_port = *(uint32_t *)(g_buffer + 28);
        if (k->msgh_id == 29901 && n >= 40 && (k->msgh_bits & MACH_MSGH_BITS_COMPLEX))
            gl_ws_connection(*(uint32_t *)(g_buffer + 28), g_buffer, n);     /* WindowServer connection (gl.c) */
        if (basic_info) host_info_reply(g_buffer, n);
        if (ats_query) {
            ats_show("from", g_buffer, n, 0);
            if (n <= receive) ats_reply(g_buffer, n, ats_type, &ats_copy);
        }
        if (n + 8 <= g_buffer_size) t = *(uint32_t *)(g_buffer + n + 4);       /* trailer size */
        if (n + t > receive + 256) t = 0;
        msg_swap(g_buffer, n, 0);
        {
            uint32_t o;
            for (o = 0; o + 4 <= t; o += 4) w_swap(g_buffer, n + o);
        }
        memcpy((void *)(uintptr_t)msg, g_buffer, n + t);
    }
    return r;
}

/* Real-time scheduling requests of the guest become ordinary time sharing. Translated code is slower than the computation a
   time-constraint thread promises, and such a thread that runs over (or a fixed-priority RR/FIFO thread that spins)
   takes the only processor: the iBook froze completely (ping answered, no process ran, not even sshd) as soon as
   QuickTime's H.264 decoder started decoding. thread_policy_set (3617) with THREAD_TIME_CONSTRAINT_POLICY becomes
   THREAD_EXTENDED_POLICY with timeshare on; thread_policy (3616, behind pthread_setschedparam) with POLICY_RR/FIFO
   becomes POLICY_TIMESHARE (the base array of RR does not fit it: the call fails, which programs ignore).
   Request layout (guest byte order, MIG with NDR): flavor/policy @32, count @36, data @40. ROSETTA_REALTIME=1 passes them. */
static void realtime_demote(uint32_t m, int32_t id, uint32_t *size)
{
    static int allowed = -1, reported;
    if (allowed < 0) allowed = getenv("ROSETTA_REALTIME") != 0;
    if (allowed) return;
    if (id == 3617 && *size >= 56 && ld32(m + 32) == THREAD_TIME_CONSTRAINT_POLICY) {
        st32(m + 4, 44);
        st32(m + 32, THREAD_EXTENDED_POLICY);
        st32(m + 36, THREAD_EXTENDED_POLICY_COUNT);
        st32(m + 40, 1);                    /* timeshare */
        *size = 44;
        if (!reported++ && g_log) fprintf(stderr, "rosetta: time-constraint thread policy made time sharing\n");
    } else if (id == 3616 && *size >= 40 && (ld32(m + 32) == POLICY_RR || ld32(m + 32) == POLICY_FIFO)) {
        st32(m + 32, POLICY_TIMESHARE);
        if (!reported++ && g_log) fprintf(stderr, "rosetta: fixed-priority thread policy made time sharing\n");
    }
}

void mach_trap(Cpu *c, int n, uint32_t base)
{
    g_base = base;
    if (g_log) {
        if (g_log > 1) sys_log_stamp();
        fprintf(stderr, "[mach %d] %08x %08x %08x %08x\n", n, arg(0), arg(1), arg(2), arg(3));
    }
    switch (n) {
    case 26: c->r[EAX] = mach_reply_port(); return;
    case 27: c->r[EAX] = mach_thread_self(); return;
    case 28: c->r[EAX] = mach_task_self(); return;
    case 29: c->r[EAX] = mach_host_self(); return;
    case 31: {
        uint32_t m = arg(0), opt = arg(1), send_size = arg(2);
        int32_t id = (opt & MACH_SEND_MSG) ? (int32_t)ld32(m + 20) : 0;
        if (id == 3616 || id == 3617) realtime_demote(m, id, &send_size);
        /* vm_protect (3803) on the own task with write access: make shared-mapped file ranges private first */
        if (id == 3803 && ld32(m + 8) == mach_task_self() && arg(2) >= 48 && (ld32(m + 44) & VM_PROT_WRITE))
            sys_make_private(ld32(m + 32), ld32(m + 36));
        /* freeing or protecting in the own task (vm_deallocate, vm_protect, mach_vm_deallocate/-protect): never in the
           host library range */
        if ((id == 3802 || id == 3803) && ld32(m + 8) == mach_task_self() && arg(2) >= 40)
            sys_mem_note(c, id == 3802 ? 'd' : 'P', ld32(m + 32), ld32(m + 36));
        if ((id == 4801 || id == 4802) && ld32(m + 8) == mach_task_self() && arg(2) >= 48)
            sys_mem_note(c, id == 4801 ? 'D' : 'Q', ld32(m + 32), ld32(m + 40));
        if ((id == 3802 || id == 3803) && ld32(m + 8) == mach_task_self() && arg(2) >= 40 &&
            sys_host_range(c, ld32(m + 32), ld32(m + 36), id == 3802 ? "vm_deallocate" : "vm_protect")) {
            c->r[EAX] = KERN_INVALID_ADDRESS;
            return;
        }
        if ((id == 4801 || id == 4802) && ld32(m + 8) == mach_task_self() && arg(2) >= 48 &&
            sys_host_range(c, ld32(m + 32), ld32(m + 40), id == 4801 ? "mach_vm_deallocate" : "mach_vm_protect")) {
            c->r[EAX] = KERN_INVALID_ADDRESS;
            return;
        }
        {
            /* for the log: destination before the reply overwrites the header, and the service names of the ports
               bootstrap_look_up (404) returned */
            static struct { uint32_t port; char name[64]; } svc[64];
            static int nsvc;
            static char asked[64];
            uint32_t to = g_log && (opt & MACH_SEND_MSG) ? ld32(m + 8) : 0;
            const char *tn = "";
            int i;
            if (g_log && id == 404 && arg(2) >= 32 + 64) snprintf(asked, sizeof asked, "%s", (const char *)(uintptr_t)(m + 32));
            if (g_log > 2 && (opt & MACH_SEND_MSG)) {     /* ROSETTA_LOG=3: the words of the message (guest order) */
                uint32_t k, sz = ld32(m + 4);
                fprintf(stderr, "    send %d:", id);
                for (k = 24; k + 4 <= sz && k < 24 + 4 * 48; k += 4) fprintf(stderr, " %08x", ld32(m + k));
                fprintf(stderr, "\n");
            }
            c->r[EAX] = (uint32_t)guest_mach_msg(c, m, (mach_msg_option_t)opt, send_size, arg(3), arg(4), arg(5), arg(6));
            if (g_log) {
                for (i = 0; i < nsvc; i++) if (svc[i].port == to) { tn = svc[i].name; break; }
                if (id == 404 && c->r[EAX] == 0 && (opt & MACH_RCV_MSG) && ld32(m + 20) == 504 && (ld32(m) & 0x80000000u)) {
                    fprintf(stderr, "[bootstrap] %s -> port %x\n", asked, ld32(m + 28));
                    if (nsvc < 64) { svc[nsvc].port = ld32(m + 28); snprintf(svc[nsvc].name, sizeof svc[0].name, "%s", asked); nsvc++; }
                }
                if (g_log > 2 && (opt & MACH_RCV_MSG) && c->r[EAX] == 0) {
                    uint32_t k, sz = ld32(m + 4);
                    fprintf(stderr, "    recv %d:", (int32_t)ld32(m + 20));
                    for (k = 24; k + 4 <= sz && k < 24 + 4 * 48; k += 4) fprintf(stderr, " %08x", ld32(m + k));
                    fprintf(stderr, "\n");
                }
                if (g_log > 1) sys_log_stamp();
                fprintf(stderr, "[mach_msg] id %d to %x%s%s -> %x", id, to, *tn ? " " : "", tn, c->r[EAX]);
            }
        }
        if (g_log) {
            if ((opt & MACH_RCV_MSG) && c->r[EAX] == 0)
                fprintf(stderr, ", reply id %d size %u RetCode %d NDR %02x%02x%02x%02x", (int32_t)ld32(m + 20), ld32(m + 4),
                        ld32(m + 4) >= 36 ? (int32_t)ld32(m + 32) : 0, ld8(m + 24), ld8(m + 25), ld8(m + 28), ld8(m + 29));
            fprintf(stderr, "\n");
        }
        return;
    }
    case 33: c->r[EAX] = (uint32_t)semaphore_signal(arg(0)); return;
    case 34: c->r[EAX] = (uint32_t)semaphore_signal_all(arg(0)); return;
    case 35: c->r[EAX] = (uint32_t)semaphore_signal_thread(arg(0), arg(1)); return;   /* pthread_cond_signal */
    case 36: case 37: case 38: case 39: case 59: case 60: case 61: case 90: {
        /* wait or yield: the thread lock is released so the thread being waited for runs */
        uint32_t a0 = arg(0), a1 = arg(1), a2 = arg(2), a3 = n == 39 ? arg(3) : 0, trunc = g_base;
        uint64_t until = n == 90 ? arg64(0) : 0;
        mach_timespec_t t, t2;
        Cpu *fc = glock_release();
        kern_return_t k;
        t.tv_sec = a1; t.tv_nsec = (clock_res_t)a2;
        t2.tv_sec = a2; t2.tv_nsec = (clock_res_t)a3;     /* 39: wait, signal, sec, nsec */
        switch (n) {
        case 36: k = semaphore_wait(a0); break;
        case 37: k = semaphore_wait_signal(a0, a1); break;
        case 38: k = semaphore_timedwait(a0, t); break;
        case 39: k = semaphore_timedwait_signal(a0, a1, t2); break;
        case 59: k = swtch_pri((int)a0); break;
        case 60: k = swtch(); break;
        case 61: k = thread_switch(a0, (int)a1, a2); break;
        default: k = mach_wait_until(until); break;
        }
        glock_acquire(fc);
        g_base = trunc;
        c->r[EAX] = (uint32_t)k;
        return;
    }
    case 62: {                              /* clock_sleep_trap(clock, type, sec, nsec, wakeup *): emulators' timers */
        uint32_t a0 = arg(0), a1 = arg(1), a2 = arg(2), a3 = arg(3), out = arg(4), trunc = g_base;
        mach_timespec_t w = { 0, 0 }, t;
        Cpu *fc = glock_release();
        kern_return_t k;
        t.tv_sec = a2; t.tv_nsec = (clock_res_t)a3;
        k = clock_sleep_trap(a0, (sleep_type_t)a1, t.tv_sec, t.tv_nsec, &w);
        glock_acquire(fc);
        g_base = trunc;
        if (out) { st32(out, w.tv_sec); st32(out + 4, (uint32_t)w.tv_nsec); }
        c->r[EAX] = (uint32_t)k;
        return;
    }
    case 43: {                              /* map_fd(fd, offset, va *, findspace, size): old file mapping (lipo, nm) */
        vm_offset_t va = ld32(arg(2));
        c->r[EAX] = (uint32_t)map_fd((int)arg(0), arg(1), &va, (boolean_t)arg(3), arg(4));
        if (c->r[EAX] == KERN_SUCCESS) { st32(arg(2), (uint32_t)va); jit_new_mapped((uint32_t)va, arg(4)); }
        return;
    }
    case 44: case 45: {                     /* task_name_for_pid, task_for_pid(target, pid, port *) (ps, top) */
        mach_port_name_t t = 0;
        c->r[EAX] = (uint32_t)(n == 45 ? task_for_pid(arg(0), (int)arg(1), &t) : task_name_for_pid(arg(0), (int)arg(1), &t));
        if (c->r[EAX] == KERN_SUCCESS && arg(2)) st32(arg(2), t);
        return;
    }
    case 46: {                              /* pid_for_task(task, int *pid) */
        int pid = 0;
        c->r[EAX] = (uint32_t)pid_for_task(arg(0), &pid);
        if (arg(1)) st32(arg(1), (uint32_t)pid);
        return;
    }
    case 89: {                              /* mach_timebase_info(info *) */
        mach_timebase_info_data_t i;
        c->r[EAX] = (uint32_t)mach_timebase_info(&i);
        st32(arg(0), i.numer);
        st32(arg(0) + 4, i.denom);
        return;
    }
    case 91: c->r[EAX] = mk_timer_create(); return;
    case 92: c->r[EAX] = (uint32_t)mk_timer_destroy(arg(0)); return;
    case 93: c->r[EAX] = (uint32_t)mk_timer_arm(arg(0), arg64(1)); return;
    case 94: {
        uint64_t t = 0;
        c->r[EAX] = (uint32_t)mk_timer_cancel(arg(0), &t);
        if (arg(1)) st64(arg(1), t);
        return;
    }
    default:
        fprintf(stderr, "rosetta: Mach trap %d not implemented yet (eip %08x)\n", n, c->eip);
        c->r[EAX] = (uint32_t)KERN_FAILURE;
    }
}
