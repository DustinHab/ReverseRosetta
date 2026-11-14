/* prebind.c - custom prebinding for the i386 libraries of the Intel root.
 *
 * Leopard's i386 libraries are not built prebound (redo_prebinding refuses: "file is not prebound"), so dyld binds
 * everything at every start: under rosetta 13000 fixups and 9300 jump table entries for Foundation, one fifth of
 * the startup time. dyld skips this for an image if it carries MH_PREBOUND, is at its linked address
 * (intel_rebase.sh), uses the two-level namespace and the timestamps of its dependencies (LC_LOAD_DYLIB) match their
 * LC_ID_DYLIB (ImageLoaderMachO::usablePrebinding, setupLazyPointerHandler).
 *
 * For every image, symbols are resolved as in dyld (resolveUndefined, findExportedSymbol with re-exports) and the
 * following is written:
 *  - external relocations: content = addend + symbol address,
 *  - non-lazy pointers (S_NON_LAZY_SYMBOL_POINTERS): symbol address,
 *  - jump tables (S_SYMBOL_STUBS, self-modifying, 5 bytes): jmp rel32 to the target,
 *  - n_value of undefined symbols = bound address (like redo_prebinding: if the prebinding becomes invalid, dyld
 *    uses it to compute the addend back), lazy pointers stay untouched (without PB_LA_PTR relocations dyld could
 *    not reset them; they bind on demand as before),
 *  - timestamps of the dependencies, MH_PREBOUND.
 * Running it again is allowed (addend = content - old n_value). Images with flat lookup, references to the program
 * or unresolvable symbols stay unchanged.
 *
 * prebind ROOT LIST [-n]   LIST: paths of the libraries (one per line), -n: only check, write nothing
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define MAX_IMAGES 2048
#define MAX_DEP 128
#define MAX_SEG 16
#define MAX_SEC 128

typedef struct Image Image;
struct Image {
    char path[1024], *name;
    uint8_t *d;                     /* whole file */
    size_t size;
    uint32_t s;                     /* start of the i386 slice */
    uint32_t flags, ncmds;
    int nseg, nsec;
    struct { uint32_t vmaddr, vmsize, fileoff, filesize; } seg[MAX_SEG];
    struct { uint32_t addr, size, flags, r1, r2; } sec[MAX_SEC];
    uint32_t symoff, nsyms, stroff, strsize, iextdef, nextdef, indoff, nind, extreloff, nextrel;
    uint32_t id_time;
    int ndep;
    struct { const char *name; uint32_t time_loc; int reexport; Image *b; } dep[MAX_DEP];
    const char *umbrella;           /* LC_SUB_FRAMEWORK */
    const char *sub_umbrella[16], *sub_library[16];
    int nsu, nsl;
    /* exports: open-addressing hash table of symbol indices */
    uint32_t *exp, expmask;
    int checked, good;
};

static Image *g_image[MAX_IMAGES];
static int g_n;
static const char *g_root;

static uint32_t l32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t l16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void s32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t b32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static uint32_t scatter(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

/* symbol table: entry i (nlist: n_strx 4, n_type 1, n_sect 1, n_desc 2, n_value 4) */
static uint8_t *sym(Image *b, uint32_t i) { return b->d + b->s + b->symoff + 12 * i; }
static const char *symname(Image *b, uint32_t i)
{
    uint32_t x = l32(sym(b, i));
    return x < b->strsize ? (const char *)b->d + b->s + b->stroff + x : "";
}

/* file offset of an address (0: not in the file) */
static uint8_t *loc(Image *b, uint32_t a, uint32_t n)
{
    int i;
    for (i = 0; i < b->nseg; i++)
        if (a >= b->seg[i].vmaddr && a + n <= b->seg[i].vmaddr + b->seg[i].filesize)
            return b->d + b->s + b->seg[i].fileoff + (a - b->seg[i].vmaddr);
    return 0;
}

static Image *has_load(const char *path)
{
    Image *b = calloc(1, sizeof *b);
    struct stat st;
    int fd = open(path, O_RDONLY);
    uint32_t lc, k;
    if (!b || fd < 0 || fstat(fd, &st)) { if (fd >= 0) close(fd); free(b); return 0; }
    b->size = (size_t)st.st_size;
    b->d = malloc(b->size);
    if (!b->d || read(fd, b->d, b->size) != (ssize_t)b->size) { close(fd); free(b->d); free(b); return 0; }
    close(fd);
    snprintf(b->path, sizeof b->path, "%s", path);
    if (b32(b->d) == 0xcafebabe) {          /* fat: find the i386 slice */
        uint32_t n = b32(b->d + 4), i;
        for (i = 0; i < n; i++)
            if (b32(b->d + 8 + 20 * i) == 7) b->s = b32(b->d + 8 + 20 * i + 8);
        if (!b->s) goto way;
    }
    if (l32(b->d + b->s) != 0xfeedface || l32(b->d + b->s + 4) != 7 || l32(b->d + b->s + 12) != 6) goto way;
    b->ncmds = l32(b->d + b->s + 16);
    b->flags = l32(b->d + b->s + 24);
    lc = b->s + 28;
    for (k = 0; k < b->ncmds; k++) {
        uint32_t cmd = l32(b->d + lc), sz = l32(b->d + lc + 4);
        const uint8_t *c = b->d + lc;
        if (sz < 8 || lc + sz > b->size) goto way;
        switch (cmd) {
        case 1: {                           /* LC_SEGMENT */
            uint32_t ns = l32(c + 48), j;
            if (b->nseg < MAX_SEG) {
                b->seg[b->nseg].vmaddr = l32(c + 24); b->seg[b->nseg].vmsize = l32(c + 28);
                b->seg[b->nseg].fileoff = l32(c + 32); b->seg[b->nseg].filesize = l32(c + 36);
                b->nseg++;
            }
            for (j = 0; j < ns && b->nsec < MAX_SEC; j++) {
                const uint8_t *x = c + 56 + 68 * j;
                b->sec[b->nsec].addr = l32(x + 32); b->sec[b->nsec].size = l32(x + 36);
                b->sec[b->nsec].flags = l32(x + 56); b->sec[b->nsec].r1 = l32(x + 60); b->sec[b->nsec].r2 = l32(x + 64);
                b->nsec++;
            }
            break;
        }
        case 2:                             /* LC_SYMTAB */
            b->symoff = l32(c + 8); b->nsyms = l32(c + 12); b->stroff = l32(c + 16); b->strsize = l32(c + 20);
            break;
        case 0xb:                           /* LC_DYSYMTAB */
            b->iextdef = l32(c + 16); b->nextdef = l32(c + 20);
            b->indoff = l32(c + 56); b->nind = l32(c + 60); b->extreloff = l32(c + 64); b->nextrel = l32(c + 68);
            break;
        case 0xd:                           /* LC_ID_DYLIB */
            b->name = (char *)c + l32(c + 8);
            b->id_time = l32(c + 12);
            break;
        case 0xc: case 0x80000018u: case 0x8000001fu: case 0x20:   /* LOAD, LOAD_WEAK, REEXPORT, LAZY_LOAD */
            if (b->ndep >= MAX_DEP) goto way;
            b->dep[b->ndep].name = (const char *)c + l32(c + 8);
            b->dep[b->ndep].time_loc = lc + 12;
            b->dep[b->ndep].reexport = cmd == 0x8000001fu;
            b->ndep++;
            break;
        case 0x12:                          /* LC_SUB_FRAMEWORK */
            b->umbrella = (const char *)c + l32(c + 8);
            break;
        case 0x13: if (b->nsu < 16) b->sub_umbrella[b->nsu++] = (const char *)c + l32(c + 8); break;
        case 0x15: if (b->nsl < 16) b->sub_library[b->nsl++] = (const char *)c + l32(c + 8); break;
        }
        lc += sz;
    }
    if (!b->name || !b->symoff || !b->nsyms || b->s + b->symoff + 12 * b->nsyms > b->size ||
        b->s + b->stroff + b->strsize > b->size || b->iextdef + b->nextdef > b->nsyms ||
        b->s + b->indoff + 4 * b->nind > b->size || b->s + b->extreloff + 8 * b->nextrel > b->size) goto way;
    {   /* exports (defined external symbols) into a hash table */
        uint32_t sz = 16, i;
        while (sz < 2 * b->nextdef + 16) sz <<= 1;
        b->exp = calloc(sz, 4);
        b->expmask = sz - 1;
        for (i = b->iextdef; i < b->iextdef + b->nextdef; i++) {
            uint32_t h = scatter(symname(b, i)) & b->expmask;
            while (b->exp[h]) h = (h + 1) & b->expmask;
            b->exp[h] = i + 1;
        }
    }
    return b;
way:
    free(b->d);
    free(b);
    return 0;
}

static Image *find(const char *name)
{
    int i;
    for (i = 0; i < g_n; i++) if (!strcmp(g_image[i]->name, name)) return g_image[i];
    return 0;
}

static const char *last(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

/* dyld: isSubframeworkOf / hasSubLibrary */
static int is_reexported(Image *parent, Image *kind, int lc_reexport)
{
    int i;
    if (lc_reexport) return 1;
    if (kind->umbrella && !strcmp(kind->umbrella, last(parent->name))) return 1;
    for (i = 0; i < parent->nsu; i++) if (!strcmp(parent->sub_umbrella[i], last(kind->name))) return 1;
    for (i = 0; i < parent->nsl; i++) {
        const char *leaf = last(kind->name), *point = strchr(leaf, '.');
        size_t n = point ? (size_t)(point - leaf) : strlen(leaf);
        if (!strncmp(parent->sub_library[i], leaf, n) && parent->sub_library[i][n] == 0) return 1;
    }
    return 0;
}

/* dyld: findExportedSymbol with re-exports; 1 and *val on success, -1: not supported (N_INDR) */
static int exported(Image *b, const char *name, uint32_t *val, int depth)
{
    uint32_t h = scatter(name) & b->expmask;
    int i, r;
    if (depth > 16) return 0;
    while (b->exp[h]) {
        uint32_t s = b->exp[h] - 1;
        if (!strcmp(symname(b, s), name)) {
            uint8_t type = sym(b, s)[4] & 0x0e;
            if (type != 0x0e && type != 0x02) return -1;     /* only N_SECT, N_ABS */
            *val = l32(sym(b, s) + 8);
            return 1;
        }
        h = (h + 1) & b->expmask;
    }
    for (i = 0; i < b->ndep; i++)
        if (b->dep[i].b && b->dep[i].reexport && (r = exported(b->dep[i].b, name, val, depth + 1)) != 0) return r;
    return 0;
}

/* dyld: resolveUndefined (two-level); 1: *val, 0: not resolvable */
static int resolve(Image *b, uint32_t i, uint32_t *val, const char **reason)
{
    uint8_t *e = sym(b, i), type = e[4];
    uint16_t desc = l16(e + 6);
    uint32_t ord = (uint32_t)(desc >> 8) & 0xff;
    Image *target;
    int r;
    if ((type & 0x0e) == 0x0e && (type & 0x10)) { *val = l32(e + 8); return 1; }   /* N_PEXT, N_SECT: internal */
    if (ord == 0) target = b;
    else if (ord == 0xff) { *reason = "reference to the program"; return 0; }
    else if (ord == 0xfe) { *reason = "flat lookup"; return 0; }
    else if (ord <= (uint32_t)b->ndep) {
        target = b->dep[ord - 1].b;
        if (!target) { *reason = "dependency missing"; return 0; }
    } else { *reason = "ordinal too large"; return 0; }
    r = exported(target, symname(b, i), val, 0);
    if (r == 1) return 1;
    if (r < 0) { *reason = "N_INDR"; return 0; }
    if ((type & 0x0e) == 0x0e) { *val = l32(e + 8); return 1; }    /* defined, not exported: like dyld (private) */
    if (desc & 0x40) { *val = 0; return 1; }                         /* N_WEAK_REF: not found -> 0 */
    *reason = "symbol not found";
    return 0;
}

static unsigned long g_st_rel, g_st_nl, g_st_jump;

/* prebind one image; do_write = 0: only check. 1: success */
static int prebind(Image *b, int do_write)
{
    uint8_t *k = b->d + b->s;
    uint32_t base = b->seg[0].vmaddr, i, j, w;
    const char *reason = 0;
    uint8_t *old_n = 0;
    int before = (b->flags & 0x10) != 0;
    unsigned long rel = 0, nl = 0, sp = 0;
    if (!(b->flags & 0x80)) { reason = "not two-level"; goto negative; }
    if (b->flags & 0x20) { reason = "MH_SPLIT_SEGS"; goto negative; }
    for (i = 0; i < (uint32_t)b->ndep; i++)
        if (!b->dep[i].b) { reason = b->dep[i].name; goto negative; }
    /* remember the old n_value (addend for a repeated run), then resolve everything first, then write */
    old_n = malloc(4 * (size_t)b->nsyms);
    for (i = 0; i < b->nsyms; i++) s32(old_n + 4 * i, l32(sym(b, i) + 8));
    /* 1. external relocations */
    for (i = 0; i < b->nextrel; i++) {
        const uint8_t *r = k + b->extreloff + 8 * i;
        uint32_t addr = l32(r), info = l32(r + 4), sn = info & 0xffffff, v;
        uint8_t *o;
        if ((addr & 0x80000000u) || !(info >> 27 & 1) || (info >> 28) != 0 || (info >> 25 & 3) != 2 || sn >= b->nsyms) {
            reason = "unusual relocation"; goto negative;
        }
        if (!(o = loc(b, base + addr, 4))) { reason = "relocation outside the file"; goto negative; }
        if (!resolve(b, sn, &v, &reason)) goto negative;
        if (do_write) s32(o, l32(o) - (before ? l32(old_n + 4 * sn) : 0) + v);
        rel++;
    }
    /* 2. non-lazy pointers and jump tables */
    for (j = 0; j < (uint32_t)b->nsec; j++) {
        uint32_t type = b->sec[j].flags & 0xff, n, sz;
        int jump = type == 8 && (b->sec[j].flags & 0x04000000u) && b->sec[j].r2 == 5;
        if (type != 6 && !jump) continue;
        sz = jump ? 5 : 4;
        n = b->sec[j].size / sz;
        if (b->sec[j].r1 + n > b->nind) { reason = "indirect table too short"; goto negative; }
        for (i = 0; i < n; i++) {
            uint32_t a = b->sec[j].addr + sz * i, is = l32(k + b->indoff + 4 * (b->sec[j].r1 + i)), v;
            uint8_t *o;
            if (is & 0xc0000000u) continue;     /* INDIRECT_SYMBOL_LOCAL / ABS */
            if (is >= b->nsyms) { reason = "indirect symbol invalid"; goto negative; }
            if (!(o = loc(b, a, sz))) { reason = "pointer outside the file"; goto negative; }
            if (!resolve(b, is, &v, &reason)) goto negative;
            if (do_write) {
                if (jump) { o[0] = 0xe9; s32(o + 1, v - (a + 5)); }
                else s32(o, v);
            }
            if (jump) sp++; else nl++;
        }
    }
    if (do_write) {
        /* 3. n_value of undefined symbols = bound address */
        for (i = 0; i < b->nsyms; i++) {
            uint8_t *e = sym(b, i);
            uint32_t v;
            const char *g2;
            if ((e[4] & 0xe0) || (e[4] & 0x0e) != 0 || !(e[4] & 0x01)) continue;     /* only N_UNDF|N_EXT, no stab */
            if (resolve(b, i, &v, &g2)) s32(e + 8, v);
        }
        /* 4. timestamps of the dependencies, MH_PREBOUND */
        for (i = 0; i < (uint32_t)b->ndep; i++) s32(b->d + b->dep[i].time_loc, b->dep[i].b->id_time);
        s32(k + 24, b->flags | 0x10);
    }
    free(old_n);
    if (!do_write) { g_st_rel += rel; g_st_nl += nl; g_st_jump += sp; }
    return 1;
negative:
    free(old_n);
    printf("  not prebound: %s (%s)\n", b->name, reason ? reason : "?");
    return 0;
}

static int save(Image *b)
{
    char tmp[1100];
    struct stat st;
    int fd;
    snprintf(tmp, sizeof tmp, "%s.prebind", b->path);
    if (stat(b->path, &st) || (fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 07777)) < 0) return 0;
    if (write(fd, b->d, b->size) != (ssize_t)b->size) { close(fd); unlink(tmp); return 0; }
    if (close(fd) || rename(tmp, b->path)) { unlink(tmp); return 0; }
    return 1;
}

int main(int argc, char **argv)
{
    FILE *f;
    char z[1200];
    int i, j, only_verify = argc > 3 && !strcmp(argv[3], "-n"), good = 0, written = 0;
    if (argc < 3 || !(f = fopen(argv[2], "r"))) { fprintf(stderr, "prebind ROOT LIST [-n]\n"); return 1; }
    g_root = argv[1];
    while (fgets(z, sizeof z, f) && g_n < MAX_IMAGES) {
        Image *b;
        z[strcspn(z, "\n")] = 0;
        if (z[0] && (b = has_load(z))) g_image[g_n++] = b;
    }
    fclose(f);
    printf("%d i386 libraries read\n", g_n);
    /* dependencies via the install name; re-exports like dyld */
    for (i = 0; i < g_n; i++) {
        Image *b = g_image[i];
        for (j = 0; j < b->ndep; j++) {
            b->dep[j].b = find(b->dep[j].name);
            if (b->dep[j].b) b->dep[j].reexport = is_reexported(b, b->dep[j].b, b->dep[j].reexport);
        }
    }
    for (i = 0; i < g_n; i++) {
        Image *b = g_image[i];
        if (!prebind(b, 0)) continue;     /* check first (without writing), then write */
        good++;
        if (only_verify) continue;
        prebind(b, 1);
        if (save(b)) written++;
        else printf("  writing failed: %s\n", b->path);
    }
    printf("%d of %d prebindable, %d written; %lu relocations, %lu pointers, %lu jump table entries%s\n", good, g_n,
           written, g_st_rel, g_st_nl, g_st_jump, only_verify ? " (checked only)" : "");
    return 0;
}
