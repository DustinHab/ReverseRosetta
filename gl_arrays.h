/* gl_arrays.h - part of gl.c (included after gl_table.h): client arrays, buffer objects, lists of names and the
 * strings of glShaderSource.
 *
 * Vertex data in the program's memory is in Intel byte order; the driver of the host reads it in its own. The
 * gl*Pointer calls are only recorded (per context and vertex array object). A draw call converts what it needs:
 *   - arrays of bytes (colors as GL_UNSIGNED_BYTE, edge flags) go to the host as they are;
 *   - client arrays of 16-, 32- and 64-bit components: the vertices the call uses, into scratch memory; glDrawArrays
 *     in pieces that stay in the cache (client_draw);
 *   - arrays in a buffer object: every buffer keeps a copy in Intel byte order (the program's data); the host's
 *     buffer of the same name holds the data converted for the attribute layout of the last draw. A static buffer
 *     is converted once, a draw with the same layout costs nothing extra; glBufferSubData of a buffer whose layout
 *     is known converts only the changed vertices (into the mapped host buffer). glMapBuffer hands out the copy.
 * Pixel buffers (GL_PIXEL_PACK/UNPACK_BUFFER) stay in Intel byte order on the host: the context swaps pixels. */

#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_PIXEL_UNPACK_BUFFER 0x88EC

static struct {
    int vertex_pointer, normal_pointer, color_pointer, index_pointer, tex_coord_pointer, edge_flag_pointer;
    int secondary_color_pointer, fog_coord_pointer, vertex_attrib_pointer_ARB, enable_client_state;
    int disable_client_state, client_active_texture, enable_vertex_attrib_array_ARB, disable_vertex_attrib_array_ARB;
    int push_client_attrib, pop_client_attrib, draw_arrays, draw_elements, draw_range_elements, bind_buffer;
    int buffer_data, buffer_sub_data, delete_buffers, get_pointerv, get_vertex_attrib_pointerv_ARB, call_lists;
    int shader_source_ARB, color4f, secondary_color3f, normal3f, fog_coordf, index_f, edge_flag, multi_tex_coord4f;
    int vertex4f, vertex_attrib4f_ARB, map_buffer, unmap_buffer, buffer_parameteri_APPLE;
    int flush_mapped_buffer_range_APPLE;
} S;

static void arrays_slots(void)
{
#define SL(f) S.f = slot_of(#f)
    SL(vertex_pointer); SL(normal_pointer); SL(color_pointer); SL(index_pointer); SL(tex_coord_pointer);
    SL(edge_flag_pointer); SL(secondary_color_pointer); SL(fog_coord_pointer); SL(vertex_attrib_pointer_ARB);
    SL(enable_client_state); SL(disable_client_state); SL(client_active_texture); SL(enable_vertex_attrib_array_ARB);
    SL(disable_vertex_attrib_array_ARB); SL(push_client_attrib); SL(pop_client_attrib); SL(draw_arrays);
    SL(draw_elements); SL(draw_range_elements); SL(bind_buffer); SL(buffer_data); SL(buffer_sub_data);
    SL(delete_buffers); SL(get_pointerv); SL(get_vertex_attrib_pointerv_ARB); SL(call_lists); SL(shader_source_ARB);
    S.color4f = slot_of("color4f"); S.secondary_color3f = slot_of("secondary_color3f"); S.normal3f = slot_of("normal3f");
    S.fog_coordf = slot_of("fog_coordf"); S.index_f = slot_of("indexf"); S.edge_flag = slot_of("edge_flag");
    S.multi_tex_coord4f = slot_of("multi_tex_coord4f"); S.vertex4f = slot_of("vertex4f");
    S.vertex_attrib4f_ARB = slot_of("vertex_attrib4f_ARB");
    S.map_buffer = slot_of("map_buffer"); S.unmap_buffer = slot_of("unmap_buffer");
    SL(buffer_parameteri_APPLE); SL(flush_mapped_buffer_range_APPLE);
#undef SL
}

/* milliseconds of a mach_absolute_time difference (ROSETTA_GL_LOG=3) */
static double ms_of(uint64_t d)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)d * tb.numer / tb.denom / 1e6;
}

/* host calls through the dispatch table of the context */
#define HV(slot, ...) ((void (*)())x->disp[slot])(x->rend, __VA_ARGS__)

/* ---------- state ---------- */

enum { AR_VERTEX, AR_NORMAL, AR_COLOR, AR_SECOND, AR_FOG, AR_INDEX, AR_EDGE, AR_TEX, AR_ATTR = AR_TEX + 8,
       AR_N = AR_ATTR + 16 };
typedef struct { uint32_t ptr, buf; int size, type, stride, norm, on; } Arr;     /* ptr: guest address or offset */
typedef struct Vao { uint32_t name; Arr a[AR_N]; struct Vao *next; } Vao;
typedef struct { uint32_t off, stride; uint8_t tsz, comps; } Spec;             /* attribute layout in a buffer */
typedef struct Buf {
    uint32_t name, size;
    uint8_t *shadow;                            /* the program's data (Intel byte order) */
    int nspec;                                  /* layout the host's copy has (0: raw) */
    Spec spec[AR_N];
    int stale;                                  /* host copy behind the shadow */
    struct Buf *next;
} Buf;
#define BUF_HASH 256
typedef struct { Buf *h[BUF_HASH]; int refs; } BufTab;
typedef struct GlState {
    GlCx *x;
    Vao def, *cur, *vaos;
    int ctex;                                   /* client active texture unit */
    uint32_t abuf, ebuf, pack, unpack;          /* bound buffers */
    uint32_t host_on;                           /* enabled arrays on the host: bit per array */
    int swap_unpack, swap_pack;                 /* GL_UNPACK/PACK_SWAP_BYTES as the guest set them */
    BufTab *bufs;
    struct { Vao v; int ctex; uint32_t abuf, ebuf; int swap_unpack, swap_pack; } stack[16];
    int sp;
    struct GlState *next;
} GlState;
static GlState *g_states, *g_state_last;

static GlState *state_get(GlCx *x)
{
    GlState *s;
    if (g_state_last && g_state_last->x == x) return g_state_last;
    for (s = g_states; s; s = s->next) if (s->x == x) return g_state_last = s;
    s = calloc(1, sizeof *s);
    if (!s) return 0;
    s->x = x;
    s->cur = &s->def;
    s->bufs = calloc(1, sizeof *s->bufs);
    if (s->bufs) s->bufs->refs = 1;
    s->next = g_states;
    g_states = s;
    return g_state_last = s;
}

/* a new context that shares objects with another one shares its buffers */
static void state_share(GlCx *x, GlCx *with)
{
    GlState *s = state_get(x), *w = with ? state_get(with) : 0;
    if (!s || !w || !w->bufs) return;
    if (s->bufs && --s->bufs->refs <= 0) free(s->bufs);
    s->bufs = w->bufs;
    s->bufs->refs++;
}

static void bufs_free(BufTab *t)
{
    int i;
    for (i = 0; i < BUF_HASH; i++)
        while (t->h[i]) { Buf *b = t->h[i]; t->h[i] = b->next; free(b->shadow); free(b); }
    free(t);
}

static void state_free(GlCx *x)
{
    GlState **q;
    for (q = &g_states; *q; q = &(*q)->next)
        if ((*q)->x == x) {
            GlState *s = *q;
            *q = s->next;
            while (s->vaos) { Vao *v = s->vaos; s->vaos = v->next; free(v); }
            if (s->bufs && --s->bufs->refs <= 0) bufs_free(s->bufs);
            if (g_state_last == s) g_state_last = 0;
            free(s);
            return;
        }
}

static Buf *buf_get(GlState *s, uint32_t name, int make)
{
    Buf *b;
    if (!name || !s->bufs) return 0;
    for (b = s->bufs->h[name & (BUF_HASH - 1)]; b; b = b->next) if (b->name == name) return b;
    if (!make || !(b = calloc(1, sizeof *b))) return 0;
    b->name = name;
    b->next = s->bufs->h[name & (BUF_HASH - 1)];
    s->bufs->h[name & (BUF_HASH - 1)] = b;
    return b;
}

static uint32_t *bound(GlState *s, uint32_t target)
{
    switch (target) {
    case GL_ARRAY_BUFFER: return &s->abuf;
    case GL_ELEMENT_ARRAY_BUFFER: return &s->ebuf;
    case GL_PIXEL_PACK_BUFFER: return &s->pack;
    case GL_PIXEL_UNPACK_BUFFER: return &s->unpack;
    }
    return 0;
}

static int type_size(uint32_t t)
{
    switch (t) {
    case 0x1400: case 0x1401: return 1;         /* byte */
    case 0x1402: case 0x1403: case 0x140B: return 2;    /* short, half float */
    case 0x1404: case 0x1405: case 0x1406: return 4;    /* int, float */
    case 0x140A: return 8;                      /* double */
    }
    return 4;
}

/* ---------- byte order of vertex data ---------- */

/* m bytes from guest address s (any alignment) to d (16-byte aligned), every word (sz 4) or halfword (sz 2) swapped:
   AltiVec, one vperm per 16 bytes (the alignment and the swap in one pattern) */
static int g_dcbz = -1;                         /* ROSETTA_GL_NO_DCBZ: let the cache fetch destination lines */
static void swap_into(uint8_t *d, uint32_t s, uint32_t m, int sz, int zero)
{
    const vector unsigned char sw4 = { 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12 };
    const vector unsigned char sw2 = { 1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14 };
    const unsigned char *p = (const unsigned char *)(uintptr_t)s;
    vector unsigned char al = vec_lvsl(0, p), mk = vec_perm(al, al, sz == 4 ? sw4 : sw2);
    uint32_t i = 0;
    if (g_dcbz < 0) g_dcbz = !getenv("ROSETTA_GL_NO_DCBZ");
    if (zero && g_dcbz && m >= 64) {
        /* whole cache lines (32 bytes) of the destination: dcbz takes them without reading them from memory first,
           half the memory traffic; prefetch the source a few lines ahead */
        if ((uintptr_t)d & 31) { vec_st(vec_perm(vec_ld(0, p), vec_ld(15, p), mk), 0, d); i = 16; }
        for (; i + 32 <= m; i += 32) {
            __asm__ volatile("dcbz 0,%0" : : "r"(d + i) : "memory");
            __asm__ volatile("dcbt 0,%0" : : "r"(p + i + 256));
            vec_st(vec_perm(vec_ld((int)i, p), vec_ld((int)i + 15, p), mk), (int)i, d);
            vec_st(vec_perm(vec_ld((int)i + 16, p), vec_ld((int)i + 31, p), mk), (int)i + 16, d);
        }
    }
    for (; i + 16 <= m; i += 16)
        vec_st(vec_perm(vec_ld((int)i, p), vec_ld((int)i + 15, p), mk), (int)i, d);
    if (sz == 4) for (; i + 4 <= m; i += 4) *(uint32_t *)(d + i) = ld32(s + i);
    else for (; i + 2 <= m; i += 2) *(uint16_t *)(d + i) = (uint16_t)ld16(s + i);
}
static void swap_run(uint8_t *d, uint32_t s, uint32_t m, int sz) { swap_into(d, s, m, sz, 1); }

/* n elements of comps components of tsz bytes from guest address s (stride gs) packed to d */
static void conv(uint8_t *d, uint32_t s, uint32_t n, int comps, int tsz, uint32_t gs)
{
    uint32_t i, k, esz = (uint32_t)(comps * tsz);
    if ((tsz == 4 || tsz == 2) && gs == esz) {  /* the usual case: packed floats, ints or shorts */
        swap_run(d, s, n * esz, tsz);
        return;
    }
    for (i = 0; i < n; i++, s += gs, d += esz)
        switch (tsz) {
        case 1: memcpy(d, (const void *)(uintptr_t)s, esz); break;
        case 2: for (k = 0; k < (uint32_t)comps; k++) ((uint16_t *)d)[k] = (uint16_t)ld16(s + 2 * k); break;
        case 4: for (k = 0; k < (uint32_t)comps; k++) ((uint32_t *)d)[k] = ld32(s + 4 * k); break;
        default: for (k = 0; k < (uint32_t)comps; k++) ((uint64_t *)d)[k] = ld64(s + 8 * k); break;
        }
}

/* in place on host memory: swap the components of a layout from vertex v0 to v1 (exclusive) */
static void swap_spec(uint8_t *p, uint32_t size, const Spec *sp, uint32_t v0, uint32_t v1)
{
    uint32_t v, k, esz = (uint32_t)sp->tsz * sp->comps;
    if (sp->tsz == 4 && sp->stride == esz) {    /* packed words: one run */
        uint32_t o = sp->off + v0 * esz, e = sp->off + v1 * esz;
        if (e > size) e = size;
        for (; o + 4 <= e; o += 4) *(uint32_t *)(p + o) = ld32((uint32_t)(uintptr_t)(p + o));
        return;
    }
    for (v = v0; v < v1; v++) {
        uint32_t o = sp->off + v * sp->stride;
        if (o + esz > size) break;
        for (k = 0; k < sp->comps; k++) {
            uint8_t *q = p + o + k * sp->tsz;
            uint32_t qa = (uint32_t)(uintptr_t)q;
            if (sp->tsz == 2) *(uint16_t *)q = (uint16_t)ld16(qa);
            else if (sp->tsz == 4) *(uint32_t *)q = ld32(qa);
            else if (sp->tsz == 8) { uint32_t lo = ld32(qa), hi = ld32(qa + 4); ((uint32_t *)q)[0] = hi; ((uint32_t *)q)[1] = lo; }
        }
    }
}

static uint32_t spec_count(const Spec *sp, uint32_t size)
{
    uint32_t esz = (uint32_t)sp->tsz * sp->comps;
    return sp->off + esz > size || !sp->stride ? (sp->off + esz <= size) : (size - sp->off - esz) / sp->stride + 1;
}

/* field by field: a Spec has padding bytes, memcmp saw stack garbage there and converted the whole buffer again */
static int spec_same(const Spec *a, const Spec *b, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (a[i].off != b[i].off || a[i].stride != b[i].stride || a[i].tsz != b[i].tsz || a[i].comps != b[i].comps) return 0;
    return 1;
}

/* host copy of buffer b in the layout nspec/spec (converted from the shadow, uploaded); host binding is restored */
static void buf_layout(GlState *s, GlCx *x, Buf *b, const Spec *spec, int nspec)
{
    uint8_t *t;
    int i;
    if (!b->stale && b->nspec == nspec && spec_same(b->spec, spec, nspec)) return;
    if (!b->shadow || !(t = gl_tmp(15, b->size, 1))) return;
    memcpy(t, b->shadow, b->size);
    for (i = 0; i < nspec; i++) swap_spec(t, b->size, &spec[i], 0, spec_count(&spec[i], b->size));
    HV(S.bind_buffer, GL_ARRAY_BUFFER, b->name);
    HV(S.buffer_sub_data, GL_ARRAY_BUFFER, 0, b->size, t);
    HV(S.bind_buffer, GL_ARRAY_BUFFER, s->abuf);
    memcpy(b->spec, spec, sizeof *spec * (size_t)nspec);
    b->nspec = nspec;
    b->stale = 0;
}

/* ---------- arrays at a draw call ---------- */

static void *g_conv[AR_N];
static uint32_t g_conv_size[AR_N];

static int comps_of(int k, const Arr *r)
{
    if (k == AR_NORMAL) return 3;
    if (k == AR_FOG || k == AR_INDEX || k == AR_EDGE) return 1;
    return r->size;
}

/* host pointer of array k (index i at host pointer + i * stride); vertices lo..hi of client arrays converted */
static void host_pointer(GlCx *x, int k, const Arr *r, uint32_t ptr, int stride)
{
    void *p = (void *)(uintptr_t)ptr;
    if (k == AR_VERTEX) HV(S.vertex_pointer, r->size, r->type, stride, p);
    else if (k == AR_NORMAL) HV(S.normal_pointer, r->type, stride, p);
    else if (k == AR_COLOR) HV(S.color_pointer, r->size, r->type, stride, p);
    else if (k == AR_SECOND) HV(S.secondary_color_pointer, r->size, r->type, stride, p);
    else if (k == AR_FOG) HV(S.fog_coord_pointer, r->type, stride, p);
    else if (k == AR_INDEX) HV(S.index_pointer, r->type, stride, p);
    else if (k == AR_EDGE) HV(S.edge_flag_pointer, stride, p);
    else if (k < AR_ATTR) { HV(S.client_active_texture, 0x84C0 + k - AR_TEX); HV(S.tex_coord_pointer, r->size, r->type, stride, p); }
    else HV(S.vertex_attrib_pointer_ARB, k - AR_ATTR, r->size, r->type, r->norm, stride, p);
}

static uint32_t array_cap(int k)
{
    static const uint32_t cap[AR_TEX] = { 0x8074, 0x8075, 0x8076, 0x845E, 0x8457, 0x8077, 0x8079 };
    return k < AR_TEX ? cap[k] : 0x8078;
}

/* enabled state of array k on the host as the guest's (vertex array objects are rosetta's) */
static void host_enable(GlState *s, GlCx *x, int k, int on)
{
    uint32_t bit = 1u << (k < 32 ? k : 31);
    if (k >= AR_ATTR) {
        HV(on ? S.enable_vertex_attrib_array_ARB : S.disable_vertex_attrib_array_ARB, k - AR_ATTR);
        return;
    }
    if (k >= AR_TEX) HV(S.client_active_texture, 0x84C0 + k - AR_TEX);
    HV(on ? S.enable_client_state : S.disable_client_state, array_cap(k));
    if (on) s->host_on |= bit; else s->host_on &= ~bit;
}

/* set up every enabled array for vertices lo..hi; returns 0 if nothing would be drawn */
static void arrays_setup(GlState *s, GlCx *x, uint32_t lo, uint32_t hi)
{
    Spec need[AR_N];
    int k, j, nb, tex_touched = 0;
    uint32_t hbuf = s->abuf, done = 0;
    for (k = 0; k < AR_N; k++) {
        Arr *r = &s->cur->a[k];
        int on = r->on, h = k < AR_ATTR ? (int)(s->host_on >> k & 1) : -1;
        if (k >= AR_TEX && k < AR_ATTR) tex_touched |= on || h == 1;
        if (k < AR_ATTR && on != h) host_enable(s, x, k, on);
        if (!on) continue;
        {
            int tsz = type_size((uint32_t)r->type), comps = comps_of(k, r), esz = tsz * comps;
            int gs = r->stride ? r->stride : esz;
            if (r->buf) {                       /* buffer object: its layout, collected per buffer below */
                if (hbuf != r->buf) { HV(S.bind_buffer, GL_ARRAY_BUFFER, r->buf); hbuf = r->buf; }
                host_pointer(x, k, r, r->ptr, gs);
                continue;
            }
            if (hbuf) { HV(S.bind_buffer, GL_ARRAY_BUFFER, 0); hbuf = 0; }
            if (tsz == 1) { host_pointer(x, k, r, r->ptr, gs); continue; }
            {
                uint32_t n = hi - lo + 1, want = n * (uint32_t)esz;
                if (want > g_conv_size[k]) {
                    void *p = realloc(g_conv[k], want);
                    if (!p) continue;
                    g_conv[k] = p;
                    g_conv_size[k] = want;
                }
                conv(g_conv[k], r->ptr + lo * (uint32_t)gs, n, comps, tsz, (uint32_t)gs);
                host_pointer(x, k, r, (uint32_t)(uintptr_t)g_conv[k] - lo * (uint32_t)esz, esz);
            }
        }
    }
    /* layouts of the buffers in use */
    for (k = 0; k < AR_N; k++) {
        Arr *r = &s->cur->a[k];
        Buf *b;
        if (!r->on || !r->buf || (done >> (k & 31) & 1)) continue;
        if (!(b = buf_get(s, r->buf, 0))) continue;
        for (j = k, nb = 0; j < AR_N; j++) {
            Arr *q = &s->cur->a[j];
            int tsz;
            if (!q->on || q->buf != r->buf) continue;
            done |= 1u << (j & 31);
            tsz = type_size((uint32_t)q->type);
            if (tsz == 1) continue;
            need[nb].off = q->ptr;
            need[nb].tsz = (uint8_t)tsz;
            need[nb].comps = (uint8_t)comps_of(j, q);
            need[nb].stride = q->stride ? (uint32_t)q->stride : (uint32_t)(tsz * need[nb].comps);
            nb++;
        }
        buf_layout(s, x, b, need, nb);
    }
    if (hbuf != s->abuf) HV(S.bind_buffer, GL_ARRAY_BUFFER, s->abuf);
    if (tex_touched) HV(S.client_active_texture, 0x84C0 + s->ctex);
}

/* Client arrays converted piece by piece. The driver of the host copies client arrays itself in the draw call
   (also from a vertex array range or a streaming buffer object: measured, the chip does not read them where they
   are), so a converted copy of the whole range is written once by rosetta and read again by the driver, all of it
   through memory. Cut into pieces of a few ten kilobytes that go into the same scratch memory, the copy stays in the
   cache: the driver reads it from there. Independent primitives (points, lines, triangles, quads) and strips (the
   pieces overlap by two vertices, an even number apart: same triangles, same orientation) are cut; fans, loops,
   line strips (stipple) and polygons are converted in one piece. Arrays of 4-byte components that share a stride
   (interleaved structures) are converted as one block; byte arrays (colors) are read by the driver from the
   program's memory. */
typedef struct {
    struct { uint32_t base, len, stride, off; } reg[AR_N];      /* interleaved blocks: guest base, bytes per vertex */
    int nreg, reg_of[AR_N];                     /* block of array k, -1: own run (sep) or none */
    uint32_t off[AR_N];                         /* offset of a separately converted array in the scratch memory */
    uint32_t per_vertex;                        /* converted bytes per vertex */
    int nsep;
} Plan;

static int g_chunk = -1;                        /* bytes per piece (ROSETTA_GL_CHUNK, 0: never cut) */
static uint8_t *g_piece;
static uint32_t g_piece_size;

/* 0 if an enabled array is in a buffer object or nothing needs converting */
static int plan_make(GlState *s, Plan *p)
{
    int k, j;
    p->nreg = p->nsep = 0;
    p->per_vertex = 0;
    for (k = 0; k < AR_N; k++) {
        Arr *r = &s->cur->a[k];
        int tsz, esz;
        uint32_t gs;
        p->reg_of[k] = -1;
        if (!r->on) continue;
        if (r->buf) return 0;
        tsz = type_size((uint32_t)r->type);
        esz = tsz * comps_of(k, r);
        gs = r->stride ? (uint32_t)r->stride : (uint32_t)esz;
        if (tsz == 1) continue;
        if (tsz == 4 && gs > (uint32_t)esz && !(gs & 3)) {      /* member of an interleaved block? */
            for (j = 0; j < p->nreg; j++)
                if (p->reg[j].stride == gs && r->ptr >= p->reg[j].base && r->ptr < p->reg[j].base + gs &&
                    !((r->ptr - p->reg[j].base) & 3))
                    break;
            if (j == p->nreg) { p->reg[j].base = r->ptr; p->reg[j].len = (uint32_t)esz; p->reg[j].stride = gs; p->nreg++; }
            if (r->ptr + (uint32_t)esz > p->reg[j].base + p->reg[j].len) p->reg[j].len = r->ptr + (uint32_t)esz - p->reg[j].base;
            p->reg_of[k] = j;
            continue;
        }
        p->per_vertex += (uint32_t)esz;
        p->nsep++;
    }
    for (j = 0; j < p->nreg; j++) p->per_vertex += p->reg[j].stride;
    return p->per_vertex != 0;
}

/* scratch layout for cn vertices: block and array offsets, returns the bytes */
static uint32_t plan_layout(GlState *s, Plan *p, uint32_t cn)
{
    uint32_t total = 0;
    int k, j;
    for (j = 0; j < p->nreg; j++) {
        p->reg[j].off = total;
        total += ((cn - 1) * p->reg[j].stride + p->reg[j].len + 15) & ~15u;
    }
    for (k = 0; k < AR_N; k++) {
        Arr *r = &s->cur->a[k];
        int tsz;
        if (!r->on || p->reg_of[k] >= 0 || (tsz = type_size((uint32_t)r->type)) == 1) continue;
        p->off[k] = total;
        total += (cn * (uint32_t)(tsz * comps_of(k, r)) + 15) & ~15u;
    }
    return total;
}

/* vertices c0 .. c0 + cn - 1 converted into m (laid out by plan_layout) and the host arrays pointed there, so that
   index c0 is the first converted vertex; byte arrays too if bytes */
static void plan_run(GlState *s, GlCx *x, Plan *p, uint8_t *m, uint32_t c0, uint32_t cn, int bytes)
{
    int k, j;
    for (j = 0; j < p->nreg; j++)
        swap_run(m + p->reg[j].off, p->reg[j].base + c0 * p->reg[j].stride, (cn - 1) * p->reg[j].stride + p->reg[j].len, 4);
    for (k = 0; k < AR_N; k++) {
        Arr *r = &s->cur->a[k];
        int tsz, comps, esz;
        uint32_t gs, at;
        if (!r->on) continue;
        tsz = type_size((uint32_t)r->type);
        comps = comps_of(k, r);
        esz = tsz * comps;
        gs = r->stride ? (uint32_t)r->stride : (uint32_t)esz;
        if (tsz == 1) {
            if (bytes) host_pointer(x, k, r, r->ptr, (int)gs);
            continue;
        }
        if ((j = p->reg_of[k]) >= 0) {
            at = (uint32_t)(uintptr_t)(m + p->reg[j].off) + (r->ptr - p->reg[j].base) - c0 * gs;
            host_pointer(x, k, r, at, (int)gs);
        } else {
            conv(m + p->off[k], r->ptr + c0 * gs, cn, comps, tsz, gs);
            host_pointer(x, k, r, (uint32_t)(uintptr_t)(m + p->off[k]) - c0 * (uint32_t)esz, esz);
        }
    }
}

static uint8_t *piece_mem(uint32_t n)
{
    if (n > g_piece_size) {
        void *q = realloc(g_piece, n);
        if (!q) return 0;
        g_piece = q;
        g_piece_size = n;
    }
    return g_piece;
}

/* host enabled arrays as the guest's; 1 if a texture coordinate array was touched (client active unit changed) */
static int sync_enables(GlState *s, GlCx *x)
{
    int k, tex = 0;
    for (k = 0; k < AR_N; k++) {
        int on = s->cur->a[k].on, h = k < AR_ATTR ? (int)(s->host_on >> k & 1) : -1;
        if (k >= AR_TEX && k < AR_ATTR) tex |= on || h == 1;
        if (k < AR_ATTR && on != h) host_enable(s, x, k, on);
    }
    return tex;
}

/* glDrawArrays of client arrays in pieces; 0 if the arrays are not all client arrays (arrays_setup instead) */
static int client_draw(GlState *s, GlCx *x, uint32_t mode, uint32_t first, uint32_t count)
{
    Plan p;
    uint32_t unit, ov = 0, vc, rem;
    uint8_t *m;
    int tex;
    if (!plan_make(s, &p)) return 0;
    if (g_chunk < 0) g_chunk = getenv("ROSETTA_GL_CHUNK") ? atoi(getenv("ROSETTA_GL_CHUNK")) : 12288;
    switch (mode) {
    case 0: unit = 1; break;                    /* points */
    case 1: unit = 2; break;                    /* lines */
    case 4: unit = 3; break;                    /* triangles */
    case 7: unit = 4; break;                    /* quads */
    case 5: case 8: unit = 2; ov = 2; break;    /* triangle and quad strips */
    default: unit = 0; break;
    }
    vc = count;
    if (unit && g_chunk > 0) {
        uint32_t room = (uint32_t)g_chunk > 16u * (uint32_t)(p.nreg + p.nsep + 1) ? (uint32_t)g_chunk - 16u * (uint32_t)(p.nreg + p.nsep + 1) : 0;
        vc = room / p.per_vertex;
        vc -= vc % unit;
        if (vc < 64) vc = 64 - 64 % unit;
        if (vc > count) vc = count;
    }
    if (!(m = piece_mem(plan_layout(s, &p, vc)))) return 0;
    tex = sync_enables(s, x);
    if (s->abuf) HV(S.bind_buffer, GL_ARRAY_BUFFER, 0);
    {
        uint64_t t0 = gl_logging() >= 3 ? mach_absolute_time() : 0, tc = 0, t1 = 0;
        int pieces = 1;
        for (rem = count; ; pieces++) {
            uint32_t cn = vc < rem ? vc : rem;
            if (t0) t1 = mach_absolute_time();
            plan_run(s, x, &p, m, first, cn, rem == count);
            if (t0) tc += mach_absolute_time() - t1;
            HV(S.draw_arrays, mode, first, cn);
            if (cn >= rem) break;
            first += cn - ov;
            rem -= cn - ov;
        }
        if (t0) fprintf(stderr, "[gl] client draw %u vertices in %d pieces: %.3f ms, converting %.3f ms of it\n", count, pieces,
                        ms_of(mach_absolute_time() - t0), ms_of(tc));
    }
    if (s->abuf) HV(S.bind_buffer, GL_ARRAY_BUFFER, s->abuf);
    if (tex) HV(S.client_active_texture, 0x84C0 + s->ctex);
    return 1;
}

/* the client arrays of vertices lo..hi converted in one piece (glDrawElements); 0 as client_draw */
static int client_range(GlState *s, GlCx *x, uint32_t lo, uint32_t hi)
{
    Plan p;
    uint8_t *m;
    int tex;
    if (!plan_make(s, &p) || !(m = piece_mem(plan_layout(s, &p, hi - lo + 1)))) return 0;
    tex = sync_enables(s, x);
    if (s->abuf) HV(S.bind_buffer, GL_ARRAY_BUFFER, 0);
    plan_run(s, x, &p, m, lo, hi - lo + 1, 1);
    if (s->abuf) HV(S.bind_buffer, GL_ARRAY_BUFFER, s->abuf);
    if (tex) HV(S.client_active_texture, 0x84C0 + s->ctex);
    return 1;
}

static int any_client_wide(GlState *s)
{
    int k;
    for (k = 0; k < AR_N; k++) {
        Arr *r = &s->cur->a[k];
        if (r->on && !r->buf && type_size((uint32_t)r->type) > 1) return 1;
    }
    return 0;
}

/* ---------- recording ---------- */

static void record(GlState *s, int k, int size, uint32_t type, int stride, uint32_t ptr, int norm)
{
    Arr *r = &s->cur->a[k];
    r->size = size; r->type = (int)type; r->stride = stride; r->ptr = ptr; r->norm = norm;
    r->buf = s->abuf;
}

#define ST GlState *s = state_get(x); if (!s) return
static void gl_x_vertex_pointer(Cpu *c, GlCx *x, uint32_t a) { ST; record(s, AR_VERTEX, (int)GI(0), GI(4), (int)GI(8), GI(12), 0); (void)c; }
static void gl_x_normal_pointer(Cpu *c, GlCx *x, uint32_t a) { ST; record(s, AR_NORMAL, 3, GI(0), (int)GI(4), GI(8), 0); (void)c; }
static void gl_x_color_pointer(Cpu *c, GlCx *x, uint32_t a) { ST; record(s, AR_COLOR, (int)GI(0), GI(4), (int)GI(8), GI(12), 0); (void)c; }
static void gl_x_secondary_color_pointer(Cpu *c, GlCx *x, uint32_t a) { ST; record(s, AR_SECOND, (int)GI(0), GI(4), (int)GI(8), GI(12), 0); (void)c; }
static void gl_x_fog_coord_pointer(Cpu *c, GlCx *x, uint32_t a) { ST; record(s, AR_FOG, 1, GI(0), (int)GI(4), GI(8), 0); (void)c; }
static void gl_x_index_pointer(Cpu *c, GlCx *x, uint32_t a) { ST; record(s, AR_INDEX, 1, GI(0), (int)GI(4), GI(8), 0); (void)c; }
static void gl_x_edge_flag_pointer(Cpu *c, GlCx *x, uint32_t a) { ST; record(s, AR_EDGE, 1, 0x1401, (int)GI(0), GI(4), 0); (void)c; }
static void gl_x_tex_coord_pointer(Cpu *c, GlCx *x, uint32_t a) { ST; record(s, AR_TEX + (s->ctex & 7), (int)GI(0), GI(4), (int)GI(8), GI(12), 0); (void)c; }
static void gl_x_vertex_attrib_pointer_ARB(Cpu *c, GlCx *x, uint32_t a)
{
    ST;
    if (GI(0) < 16) record(s, AR_ATTR + (int)GI(0), (int)GI(4), GI(8), (int)GI(16), GI(20), (int)GB(12));
    (void)c;
}

static int cap_array(GlState *s, uint32_t cap)
{
    switch (cap) {
    case 0x8074: return AR_VERTEX;
    case 0x8075: return AR_NORMAL;
    case 0x8076: return AR_COLOR;
    case 0x8077: return AR_INDEX;
    case 0x8078: return AR_TEX + (s->ctex & 7);
    case 0x8079: return AR_EDGE;
    case 0x8457: return AR_FOG;
    case 0x845E: return AR_SECOND;
    }
    return -1;
}
static void client_state(Cpu *c, GlCx *x, uint32_t a, int on)
{
    int k;
    ST;
    /* GL_VERTEX_ARRAY_RANGE_APPLE: the driver would read the program's memory (Intel byte order) itself; the
       arrays are converted at the draw call instead, so the range stays off on the host */
    if (GI(0) == 0x851D) return;
    k = cap_array(s, GI(0));
    if (k < 0) { HV(on ? S.enable_client_state : S.disable_client_state, GI(0)); return; }
    s->cur->a[k].on = on;
    host_enable(s, x, k, on);
    if (k >= AR_TEX) HV(S.client_active_texture, 0x84C0 + s->ctex);
    (void)c;
}
static void gl_x_enable_client_state(Cpu *c, GlCx *x, uint32_t a) { client_state(c, x, a, 1); }
static void gl_x_disable_client_state(Cpu *c, GlCx *x, uint32_t a) { client_state(c, x, a, 0); }
static void gl_x_client_active_texture(Cpu *c, GlCx *x, uint32_t a)
{
    ST;
    s->ctex = (int)(GI(0) - 0x84C0) & 7;
    HV(S.client_active_texture, GI(0));
    (void)c;
}
static void gl_x_enable_vertex_attrib_array_ARB(Cpu *c, GlCx *x, uint32_t a)
{
    ST;
    if (GI(0) < 16) s->cur->a[AR_ATTR + GI(0)].on = 1;
    HV(S.enable_vertex_attrib_array_ARB, GI(0));
    (void)c;
}
static void gl_x_disable_vertex_attrib_array_ARB(Cpu *c, GlCx *x, uint32_t a)
{
    ST;
    if (GI(0) < 16) s->cur->a[AR_ATTR + GI(0)].on = 0;
    HV(S.disable_vertex_attrib_array_ARB, GI(0));
    (void)c;
}

static void gl_x_interleaved_arrays(Cpu *c, GlCx *x, uint32_t a)
{
    /* GL 1.1 table 2.5: texture, color, normal, vertex sizes and offsets */
    static const struct { short st, sc, ct, sv, pc, pn, pv, size; } f[14] = {
        { 0, 0, 0, 2, 0, 0, 0, 8 }, { 0, 0, 0, 3, 0, 0, 0, 12 }, { 0, 4, 1, 2, 0, 0, 4, 12 }, { 0, 4, 1, 3, 0, 0, 4, 16 },
        { 0, 3, 0, 3, 0, 0, 12, 24 }, { 0, 0, 0, 3, 0, 0, 12, 24 }, { 0, 4, 0, 3, 0, 16, 28, 40 },
        { 2, 0, 0, 3, 0, 0, 8, 20 }, { 4, 0, 0, 4, 0, 0, 16, 32 }, { 2, 4, 1, 3, 8, 0, 12, 24 },
        { 2, 3, 0, 3, 8, 0, 20, 32 }, { 2, 0, 0, 3, 0, 8, 20, 32 }, { 2, 4, 0, 3, 8, 24, 36, 48 },
        { 4, 4, 0, 4, 16, 32, 44, 60 },
    };
    uint32_t fmt = GI(0) - 0x2A20, p = GI(8);
    int stride = (int)GI(4), k;
    ST;
    if (fmt >= 14) return;
    if (!stride) stride = f[fmt].size;
    k = AR_TEX + (s->ctex & 7);
    s->cur->a[AR_EDGE].on = 0; host_enable(s, x, AR_EDGE, 0);
    s->cur->a[AR_INDEX].on = 0; host_enable(s, x, AR_INDEX, 0);
    s->cur->a[k].on = f[fmt].st != 0;
    if (f[fmt].st) record(s, k, f[fmt].st, 0x1406, stride, p, 0);
    s->cur->a[AR_COLOR].on = f[fmt].sc != 0;
    if (f[fmt].sc) record(s, AR_COLOR, f[fmt].sc, f[fmt].ct ? 0x1401 : 0x1406, stride, p + (uint32_t)f[fmt].pc, 0);
    s->cur->a[AR_NORMAL].on = f[fmt].pn != 0 || fmt == 5;
    if (s->cur->a[AR_NORMAL].on) record(s, AR_NORMAL, 3, 0x1406, stride, p + (uint32_t)f[fmt].pn, 0);
    s->cur->a[AR_VERTEX].on = 1;
    record(s, AR_VERTEX, f[fmt].sv, 0x1406, stride, p + (uint32_t)f[fmt].pv, 0);
    host_enable(s, x, k, s->cur->a[k].on);
    HV(S.client_active_texture, 0x84C0 + s->ctex);
    host_enable(s, x, AR_COLOR, s->cur->a[AR_COLOR].on);
    host_enable(s, x, AR_NORMAL, s->cur->a[AR_NORMAL].on);
    host_enable(s, x, AR_VERTEX, 1);
    (void)c;
}

static void gl_x_push_client_attrib(Cpu *c, GlCx *x, uint32_t a)
{
    ST;
    if (s->sp < 16) {
        s->stack[s->sp].v = *s->cur;
        s->stack[s->sp].ctex = s->ctex;
        s->stack[s->sp].abuf = s->abuf;
        s->stack[s->sp].ebuf = s->ebuf;
        s->stack[s->sp].swap_unpack = s->swap_unpack;
        s->stack[s->sp].swap_pack = s->swap_pack;
    }
    s->sp++;
    HV(S.push_client_attrib, GI(0));
    (void)c;
}
static void gl_x_pop_client_attrib(Cpu *c, GlCx *x, uint32_t a)
{
    ST;
    ((void (*)(void *))x->disp[S.pop_client_attrib])(x->rend);
    if (s->sp > 0 && --s->sp < 16) {
        Vao *keep = s->cur->next;
        uint32_t name = s->cur->name;
        *s->cur = s->stack[s->sp].v;
        s->cur->next = keep;
        s->cur->name = name;
        s->ctex = s->stack[s->sp].ctex;
        s->abuf = s->stack[s->sp].abuf;
        s->ebuf = s->stack[s->sp].ebuf;
        s->swap_unpack = s->stack[s->sp].swap_unpack;
        s->swap_pack = s->stack[s->sp].swap_pack;
        s->host_on = 0xffffffffu;               /* unknown: arrays_setup sets each one again */
        {
            int k;
            for (k = 0; k < AR_ATTR; k++) host_enable(s, x, k, s->cur->a[k].on);
            HV(S.client_active_texture, 0x84C0 + s->ctex);
        }
    }
    (void)c; (void)a;
}

/* ---------- drawing ---------- */

static void gl_x_draw_arrays(Cpu *c, GlCx *x, uint32_t a)
{
    int32_t first = (int32_t)GI(4), count = (int32_t)GI(8);
    ST;
    if (count <= 0 || first < 0) { HV(S.draw_arrays, GI(0), first, count); return; }
    if (client_draw(s, x, GI(0), (uint32_t)first, (uint32_t)count)) return;
    arrays_setup(s, x, (uint32_t)first, (uint32_t)(first + count - 1));
    HV(S.draw_arrays, GI(0), first, count);
    (void)c;
}

/* indices: client ones converted (and their range), from a buffer: its copy for the range, the buffer converted */
static void *indices(GlState *s, GlCx *x, uint32_t type, uint32_t count, uint32_t ptr, uint32_t *lo, uint32_t *hi, int range)
{
    int tsz = type == 0x1401 ? 1 : type == 0x1403 ? 2 : 4;
    uint32_t i, mn = 0xffffffffu, mx = 0;
    const uint8_t *src;
    Buf *b = s->ebuf ? buf_get(s, s->ebuf, 0) : 0;
    if (s->ebuf) {
        if (b && tsz > 1) {
            Spec sp;
            sp.off = 0; sp.stride = (uint32_t)tsz; sp.tsz = (uint8_t)tsz; sp.comps = 1;
            buf_layout(s, x, b, &sp, 1);
        }
        if (range && b && b->shadow && ptr + count * (uint32_t)tsz <= b->size) {
            src = b->shadow + ptr;
            for (i = 0; i < count; i++) {
                uint32_t v = tsz == 1 ? src[i] : tsz == 2 ? ld16((uint32_t)(uintptr_t)src + 2 * i) : ld32((uint32_t)(uintptr_t)src + 4 * i);
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            *lo = mn; *hi = mx;
        }
        return (void *)(uintptr_t)ptr;
    }
    {
        uint8_t *t = gl_tmp(14, count, (uint32_t)tsz);
        if (!t) return 0;
        for (i = 0; i < count; i++) {
            uint32_t v;
            if (tsz == 1) { v = ld8(ptr + i); t[i] = (uint8_t)v; }
            else if (tsz == 2) { v = ld16(ptr + 2 * i); ((uint16_t *)t)[i] = (uint16_t)v; }
            else { v = ld32(ptr + 4 * i); ((uint32_t *)t)[i] = v; }
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        *lo = mn; *hi = mx;
        return t;
    }
}

static void draw_elements(GlState *s, GlCx *x, uint32_t mode, uint32_t count, uint32_t type, uint32_t ptr,
                          int have_range, uint32_t start, uint32_t end)
{
    uint32_t lo = 0, hi = 0;
    void *ix;
    if (!count) return;
    ix = indices(s, x, type, count, ptr, &lo, &hi, !have_range && any_client_wide(s));
    if (have_range) { lo = start; hi = end; }
    if (hi < lo) hi = lo;
    if (hi - lo > (1u << 24)) hi = lo + (1u << 24);       /* nonsense range: the driver reports it */
    if (!(any_client_wide(s) && client_range(s, x, lo, hi))) arrays_setup(s, x, lo, hi);
    if (s->ebuf) HV(S.bind_buffer, GL_ELEMENT_ARRAY_BUFFER, s->ebuf);
    if (have_range) HV(S.draw_range_elements, mode, start, end, count, type, ix);
    else HV(S.draw_elements, mode, count, type, ix);
}
static void gl_x_draw_elements(Cpu *c, GlCx *x, uint32_t a)
{
    ST;
    draw_elements(s, x, GI(0), GI(4), GI(8), GI(12), 0, 0, 0);
    (void)c;
}
static void gl_x_draw_range_elements(Cpu *c, GlCx *x, uint32_t a)
{
    ST;
    draw_elements(s, x, GI(0), GI(12), GI(16), GI(20), 1, GI(4), GI(8));
    (void)c;
}
static void gl_x_multi_draw_arrays(Cpu *c, GlCx *x, uint32_t a)
{
    uint32_t i, n = GI(12), fp = GI(4), cp = GI(8);
    ST;
    for (i = 0; i < n; i++) {
        int32_t first = (int32_t)ld32(fp + 4 * i), count = (int32_t)ld32(cp + 4 * i);
        if (count <= 0 || first < 0) continue;
        if (client_draw(s, x, GI(0), (uint32_t)first, (uint32_t)count)) continue;
        arrays_setup(s, x, (uint32_t)first, (uint32_t)(first + count - 1));
        HV(S.draw_arrays, GI(0), first, count);
    }
    (void)c;
}
static void gl_x_multi_draw_elements(Cpu *c, GlCx *x, uint32_t a)
{
    uint32_t i, n = GI(16), cp = GI(4), ip = GI(12);
    ST;
    for (i = 0; i < n; i++) draw_elements(s, x, GI(0), ld32(cp + 4 * i), GI(8), ld32(ip + 4 * i), 0, 0, 0);
    (void)c;
}

/* one vertex from the arrays: as immediate commands (attributes first, the vertex last) */
static float comp(const Arr *r, uint32_t p, int k, int normalized)
{
    switch (r->type) {
    case 0x1400: { float v = (float)(int8_t)ld8(p + (uint32_t)k); return normalized ? v / 127.0f : v; }
    case 0x1401: { float v = (float)ld8(p + (uint32_t)k); return normalized ? v / 255.0f : v; }
    case 0x1402: { float v = (float)(int16_t)ld16(p + 2u * (uint32_t)k); return normalized ? v / 32767.0f : v; }
    case 0x1403: { float v = (float)ld16(p + 2u * (uint32_t)k); return normalized ? v / 65535.0f : v; }
    case 0x1404: { float v = (float)(int32_t)ld32(p + 4u * (uint32_t)k); return normalized ? v / 2147483647.0f : v; }
    case 0x1405: { float v = (float)ld32(p + 4u * (uint32_t)k); return normalized ? v / 4294967295.0f : v; }
    case 0x140A: return (float)gl_d(ld64(p + 8u * (uint32_t)k));
    }
    return gl_f(ld32(p + 4u * (uint32_t)k));
}
static void gl_x_array_element(Cpu *c, GlCx *x, uint32_t a)
{
    uint32_t i = GI(0);
    int k;
    ST;
    for (k = AR_N - 1; k >= 0; k--) {           /* vertex (0) last, generic attribute 0 means the vertex too */
        Arr *r = &s->cur->a[k];
        Buf *b;
        float v[4] = { 0, 0, 0, 1 };
        int n, j, esz;
        uint32_t p;
        if (!r->on) continue;
        n = comps_of(k, r);
        esz = type_size((uint32_t)r->type) * n;
        p = r->ptr + i * (uint32_t)(r->stride ? r->stride : esz);
        if (r->buf) {
            if (!(b = buf_get(s, r->buf, 0)) || !b->shadow || p + (uint32_t)esz > b->size) continue;
            p += (uint32_t)(uintptr_t)b->shadow;
        }
        for (j = 0; j < n && j < 4; j++) v[j] = comp(r, p, j, k == AR_COLOR || k == AR_SECOND || (k >= AR_ATTR && r->norm));
        if (k == AR_VERTEX) HV(S.vertex4f, v[0], v[1], n > 2 ? v[2] : 0.0f, n > 3 ? v[3] : 1.0f);
        else if (k == AR_NORMAL) HV(S.normal3f, v[0], v[1], v[2]);
        else if (k == AR_COLOR) HV(S.color4f, v[0], v[1], v[2], n > 3 ? v[3] : 1.0f);
        else if (k == AR_SECOND) HV(S.secondary_color3f, v[0], v[1], v[2]);
        else if (k == AR_FOG) HV(S.fog_coordf, v[0]);
        else if (k == AR_INDEX) HV(S.index_f, v[0]);
        else if (k == AR_EDGE) HV(S.edge_flag, ld8(p) != 0);
        else if (k < AR_ATTR) HV(S.multi_tex_coord4f, 0x84C0 + k - AR_TEX, v[0], n > 1 ? v[1] : 0.0f, n > 2 ? v[2] : 0.0f, n > 3 ? v[3] : 1.0f);
        else HV(S.vertex_attrib4f_ARB, k - AR_ATTR, v[0], n > 1 ? v[1] : 0.0f, n > 2 ? v[2] : 0.0f, n > 3 ? v[3] : 1.0f);
    }
    (void)c;
}

/* ---------- buffer objects ---------- */

static void gl_x_bind_buffer(Cpu *c, GlCx *x, uint32_t a)
{
    uint32_t *b;
    ST;
    if ((b = bound(s, GI(0))) != 0) *b = GI(4);
    if (GI(4)) buf_get(s, GI(4), 1);
    HV(S.bind_buffer, GI(0), GI(4));
    (void)c;
}

/* Bytes a0..a1 of the shadow (one packed attribute of tsz-byte components) converted straight into the host's buffer:
   mapped (that waits for the chip like glBufferSubData), only the range flushed. Saves the scratch copy that
   glBufferSubData copies once more (refilled buffer 7 % faster). The destination is written without dcbz: mapped
   memory may be uncached on other graphics chips. Small updates go through glBufferSubData. */
#define MAP_MIN 16384
#define GL_WRITE_ONLY 0x88B9
#define GL_BUFFER_FLUSHING_UNMAP_APPLE 0x8A13
static int map_upload(GlCx *x, Buf *b, uint32_t target, uint32_t a0, uint32_t a1, int tsz)
{
    uint8_t *m;
    HV(S.buffer_parameteri_APPLE, target, GL_BUFFER_FLUSHING_UNMAP_APPLE, 0);
    m = ((void *(*)(void *, uint32_t, uint32_t))x->disp[S.map_buffer])(x->rend, target, GL_WRITE_ONLY);
    if (!m) return 0;
    if ((uintptr_t)(m + a0) & 15) {
        uint8_t *t = gl_tmp(15, a1 - a0, 1);
        if (t) { swap_into(t, (uint32_t)(uintptr_t)(b->shadow + a0), a1 - a0, tsz, 1); memcpy(m + a0, t, a1 - a0); }
    } else swap_into(m + a0, (uint32_t)(uintptr_t)(b->shadow + a0), a1 - a0, tsz, 0);
    HV(S.flush_mapped_buffer_range_APPLE, target, a0, a1 - a0);
    ((unsigned char (*)(void *, uint32_t))x->disp[S.unmap_buffer])(x->rend, target);
    return 1;
}

/* bytes lo..hi of b changed in the shadow: to the host (converted if the layout is known) */
static void buf_changed(GlState *s, GlCx *x, Buf *b, uint32_t target, uint32_t lo, uint32_t hi)
{
    uint8_t *t;
    int i;
    if (hi > b->size) hi = b->size;
    if (lo >= hi) return;
    if (!b->nspec || target != GL_ARRAY_BUFFER) {
        if (b->nspec) b->stale = 1;             /* converted copy: rebuilt at the next draw */
        else HV(S.buffer_sub_data, target, lo, hi - lo, b->shadow + lo);
        return;
    }
    /* the vertices of every attribute that touch lo..hi, converted from the shadow */
    {
        uint32_t a0 = hi, a1 = lo;
        for (i = 0; i < b->nspec; i++) {
            Spec *sp = &b->spec[i];
            uint32_t v0, v1, esz = (uint32_t)sp->tsz * sp->comps;
            if (!sp->stride || hi <= sp->off) continue;
            v0 = lo > sp->off ? (lo - sp->off) / sp->stride : 0;
            v1 = (hi - sp->off + sp->stride - 1) / sp->stride;
            if (sp->off + v0 * sp->stride < a0) a0 = sp->off + v0 * sp->stride;
            if (sp->off + (v1 - 1) * sp->stride + esz > a1) a1 = sp->off + (v1 - 1) * sp->stride + esz;
        }
        if (lo < a0) a0 = lo;
        if (hi > a1) a1 = hi;
        if (a1 > b->size) a1 = b->size;
        if (!(t = gl_tmp(15, a1 - a0, 1))) return;
        /* the changed bytes all inside one packed attribute (an array of positions refilled): one AltiVec pass */
        for (i = 0; i < b->nspec; i++) {
            Spec *sp = &b->spec[i];
            uint32_t esz = (uint32_t)sp->tsz * sp->comps, e = sp->off + spec_count(sp, b->size) * esz;
            if ((sp->tsz == 4 || sp->tsz == 2) && sp->stride == esz && a0 >= sp->off && a1 <= e && !((a0 - sp->off) % sp->tsz) &&
                !((a1 - a0) % sp->tsz)) {
                if (a1 - a0 < MAP_MIN || !map_upload(x, b, target, a0, a1, sp->tsz)) {
                    swap_run(t, (uint32_t)(uintptr_t)(b->shadow + a0), a1 - a0, sp->tsz);
                    HV(S.buffer_sub_data, target, a0, a1 - a0, t);
                }
                return;
            }
        }
        memcpy(t, b->shadow + a0, a1 - a0);
        for (i = 0; i < b->nspec; i++) {
            Spec sp = b->spec[i];
            uint32_t v0;
            if (!sp.stride || a1 <= sp.off) continue;
            v0 = a0 > sp.off ? (a0 - sp.off + sp.stride - 1) / sp.stride : 0;       /* first whole vertex */
            sp.off = sp.off + v0 * sp.stride - a0;
            swap_spec(t, a1 - a0, &sp, 0, spec_count(&sp, a1 - a0));
        }
        HV(S.buffer_sub_data, target, a0, a1 - a0, t);
    }
}

static void gl_x_buffer_data(Cpu *c, GlCx *x, uint32_t a)
{
    uint32_t target = GI(0), size = GI(4), data = GI(8), *bn;
    Buf *b;
    ST;
    bn = bound(s, target);
    b = bn ? buf_get(s, *bn, 1) : 0;
    if (!b) { HV(S.buffer_data, target, size, (void *)(uintptr_t)data, GI(12)); return; }
    if (size != b->size || !b->shadow) {
        uint8_t *p = realloc(b->shadow, size ? size : 1);
        if (!p) { HV(S.buffer_data, target, size, (void *)0, GI(12)); return; }
        b->shadow = p;
        b->size = size;
    }
    if (data) memcpy(b->shadow, (const void *)(uintptr_t)data, size);
    else memset(b->shadow, 0, size);
    b->nspec = 0;
    b->stale = 0;
    HV(S.buffer_data, target, size, b->shadow, GI(12));      /* raw: right for pixels, converted at the draw */
    (void)c;
}

static void gl_x_buffer_sub_data(Cpu *c, GlCx *x, uint32_t a)
{
    uint32_t target = GI(0), off = GI(4), size = GI(8), data = GI(12), *bn;
    Buf *b;
    ST;
    bn = bound(s, target);
    b = bn ? buf_get(s, *bn, 0) : 0;
    if (!b || !b->shadow || off + size > b->size || off + size < off) {
        HV(S.buffer_sub_data, target, off, size, (void *)(uintptr_t)data);
        return;
    }
    memcpy(b->shadow + off, (const void *)(uintptr_t)data, size);     /* (while the chip may still draw from it) */
    buf_changed(s, x, b, target, off, off + size);
    (void)c;
}

static void gl_x_get_buffer_sub_data(Cpu *c, GlCx *x, uint32_t a)
{
    uint32_t off = GI(4), size = GI(8), *bn;
    Buf *b;
    ST;
    bn = bound(s, GI(0));
    b = bn ? buf_get(s, *bn, 0) : 0;
    if (b && b->shadow && off + size <= b->size) memcpy((void *)(uintptr_t)GI(12), b->shadow + off, size);
    (void)c;
}

static void gl_x_map_buffer(Cpu *c, GlCx *x, uint32_t a)
{
    uint32_t *bn;
    Buf *b;
    ST;
    bn = bound(s, GI(0));
    b = bn ? buf_get(s, *bn, 0) : 0;
    c->r[EAX] = b && b->shadow ? (uint32_t)(uintptr_t)b->shadow : 0;     /* the program writes into the copy */
}

static void gl_x_unmap_buffer(Cpu *c, GlCx *x, uint32_t a)
{
    uint32_t *bn;
    Buf *b;
    ST;
    bn = bound(s, GI(0));
    b = bn ? buf_get(s, *bn, 0) : 0;
    if (b && b->shadow) buf_changed(s, x, b, GI(0), 0, b->size);
    c->r[EAX] = 1;
}

static void gl_x_flush_mapped_buffer_range_APPLE(Cpu *c, GlCx *x, uint32_t a)
{
    uint32_t *bn;
    Buf *b;
    ST;
    bn = bound(s, GI(0));
    b = bn ? buf_get(s, *bn, 0) : 0;
    if (b && b->shadow) buf_changed(s, x, b, GI(0), GI(4), GI(4) + GI(8));
    (void)c;
}

static void gl_x_delete_buffers(Cpu *c, GlCx *x, uint32_t a)
{
    uint32_t i, n = GI(0), p = GI(4), *t = gl_tmp(1, n, 4);
    ST;
    if (!t) return;
    for (i = 0; i < n; i++) {
        uint32_t name = ld32(p + 4 * i);
        Buf **q;
        t[i] = name;
        if (!name || !s->bufs) continue;
        for (q = &s->bufs->h[name & (BUF_HASH - 1)]; *q; q = &(*q)->next)
            if ((*q)->name == name) { Buf *b = *q; *q = b->next; free(b->shadow); free(b); break; }
        if (s->abuf == name) s->abuf = 0;
        if (s->ebuf == name) s->ebuf = 0;
    }
    HV(S.delete_buffers, n, t);
    (void)c;
}

/* ---------- vertex array objects (rosetta's own: the host's stay at the default one) ---------- */

static void gl_x_bind_vertex_array_EXT(Cpu *c, GlCx *x, uint32_t a)
{
    uint32_t name = GI(0);
    Vao *v;
    int k;
    ST;
    if (!name) v = &s->def;
    else {
        for (v = s->vaos; v && v->name != name; v = v->next) ;
        if (!v && (v = calloc(1, sizeof *v)) != 0) { v->name = name; v->next = s->vaos; s->vaos = v; }
        if (!v) return;
    }
    s->cur = v;
    for (k = 0; k < AR_ATTR; k++) if ((int)(s->host_on >> k & 1) != v->a[k].on) host_enable(s, x, k, v->a[k].on);
    HV(S.client_active_texture, 0x84C0 + s->ctex);
    (void)c;
}
static void gl_x_delete_vertex_arrays_EXT(Cpu *c, GlCx *x, uint32_t a)
{
    uint32_t i, n = GI(0), p = GI(4);
    ST;
    for (i = 0; i < n; i++) {
        uint32_t name = ld32(p + 4 * i);
        Vao **q;
        for (q = &s->vaos; *q; q = &(*q)->next)
            if ((*q)->name == name) {
                Vao *v = *q;
                *q = v->next;
                if (s->cur == v) s->cur = &s->def;
                free(v);
                break;
            }
    }
    (void)c;
}

/* ---------- pointers asked back ---------- */

static void gl_x_get_pointerv(Cpu *c, GlCx *x, uint32_t a)
{
    uint32_t pname = GI(0), out = GI(4);
    int k = -1;
    ST;
    switch (pname) {
    case 0x808E: k = AR_VERTEX; break;
    case 0x808F: k = AR_NORMAL; break;
    case 0x8090: k = AR_COLOR; break;
    case 0x8091: k = AR_INDEX; break;
    case 0x8092: k = AR_TEX + (s->ctex & 7); break;
    case 0x8093: k = AR_EDGE; break;
    case 0x8456: k = AR_FOG; break;
    case 0x845D: k = AR_SECOND; break;
    }
    if (out) st32(out, k >= 0 ? s->cur->a[k].ptr : 0);
    (void)c;
}
static void gl_x_get_vertex_attrib_pointerv_ARB(Cpu *c, GlCx *x, uint32_t a)
{
    ST;
    if (GI(8)) st32(GI(8), GI(0) < 16 ? s->cur->a[AR_ATTR + GI(0)].ptr : 0);
    (void)c;
}

/* ---------- pixel transfers ----------
 * By default the host context swaps 16- and 32-bit components (GL_*_SWAP_BYTES the other way round than the guest
 * set it). For the most common types the driver gets a call that needs no swapping, which keeps it on its fast
 * paths: bytes have no byte order, and a packed 32-bit pixel with its bytes swapped is the same pixel in the type
 * with the other component order (GL_UNSIGNED_INT_8_8_8_8 <-> _8_8_8_8_REV). */

/* the guest's GL_UNPACK/PACK_SWAP_BYTES */
static void pixel_store_note(GlCx *x, uint32_t pname, int v)
{
    GlState *s = state_get(x);
    if (!s) return;
    if (pname == GL_UNPACK_SWAP_BYTES) s->swap_unpack = v != 0;
    else if (pname == GL_PACK_SWAP_BYTES) s->swap_pack = v != 0;
}

static void pixels(GlCx *x, uint32_t a, int slot, int n, int ti, int pack)
{
    uint32_t v[11], t, pname = pack ? GL_PACK_SWAP_BYTES : GL_UNPACK_SWAP_BYTES;
    GlState *s = state_get(x);
    int g = s ? (pack ? s->swap_pack : s->swap_unpack) : 0, want = !g, k;
    void *f = x->disp[slot];
    for (k = 0; k < n; k++) v[k] = GI(4 * k);
    t = v[ti];
    if (t == 0x8035 || t == 0x8367) { v[ti] = t == 0x8035 ? 0x8367 : 0x8035; want = g; }
    else if (t == 0x1400 || t == 0x1401 || t == 0x1A00) want = 0;     /* bytes, bitmaps */
    if (want != !g) ((void (*)(void *, uint32_t, uint32_t))x->disp[g_slot_pixel_storei])(x->rend, pname, (uint32_t)want);
    switch (n) {
    case 5: ((void (*)(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))f)(x->rend, v[0], v[1], v[2], v[3], v[4]); break;
    case 7: ((void (*)(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))f)(x->rend, v[0], v[1], v[2], v[3], v[4], v[5], v[6]); break;
    case 8: ((void (*)(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))f)(x->rend, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]); break;
    case 9: ((void (*)(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))f)(x->rend, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]); break;
    case 10: ((void (*)(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))f)(x->rend, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]); break;
    case 11: ((void (*)(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))f)(x->rend, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10]); break;
    }
    if (want != !g) ((void (*)(void *, uint32_t, uint32_t))x->disp[g_slot_pixel_storei])(x->rend, pname, (uint32_t)!g);
}
static int g_slot_tex_image1D = -1, g_slot_tex_image2D, g_slot_tex_image3D, g_slot_tex_sub_image1D,
           g_slot_tex_sub_image2D, g_slot_tex_sub_image3D, g_slot_draw_pixels, g_slot_read_pixels, g_slot_get_tex_image;
static void pixel_slots(void)
{
    g_slot_tex_image1D = slot_of("tex_image1D"); g_slot_tex_image2D = slot_of("tex_image2D");
    g_slot_tex_image3D = slot_of("tex_image3D"); g_slot_tex_sub_image1D = slot_of("tex_sub_image1D");
    g_slot_tex_sub_image2D = slot_of("tex_sub_image2D"); g_slot_tex_sub_image3D = slot_of("tex_sub_image3D");
    g_slot_draw_pixels = slot_of("draw_pixels"); g_slot_read_pixels = slot_of("read_pixels");
    g_slot_get_tex_image = slot_of("get_tex_image");
}
static void gl_x_tex_image1D(Cpu *c, GlCx *x, uint32_t a) { pixels(x, a, g_slot_tex_image1D, 8, 6, 0); (void)c; }
static void gl_x_tex_image2D(Cpu *c, GlCx *x, uint32_t a) { pixels(x, a, g_slot_tex_image2D, 9, 7, 0); (void)c; }
static void gl_x_tex_image3D(Cpu *c, GlCx *x, uint32_t a) { pixels(x, a, g_slot_tex_image3D, 10, 8, 0); (void)c; }
static void gl_x_tex_sub_image1D(Cpu *c, GlCx *x, uint32_t a) { pixels(x, a, g_slot_tex_sub_image1D, 7, 5, 0); (void)c; }
static void gl_x_tex_sub_image2D(Cpu *c, GlCx *x, uint32_t a) { pixels(x, a, g_slot_tex_sub_image2D, 9, 7, 0); (void)c; }
static void gl_x_tex_sub_image3D(Cpu *c, GlCx *x, uint32_t a) { pixels(x, a, g_slot_tex_sub_image3D, 11, 9, 0); (void)c; }
static void gl_x_draw_pixels(Cpu *c, GlCx *x, uint32_t a) { pixels(x, a, g_slot_draw_pixels, 5, 3, 0); (void)c; }
static void gl_x_read_pixels(Cpu *c, GlCx *x, uint32_t a) { pixels(x, a, g_slot_read_pixels, 7, 5, 1); (void)c; }
static void gl_x_get_tex_image(Cpu *c, GlCx *x, uint32_t a) { pixels(x, a, g_slot_get_tex_image, 5, 3, 1); (void)c; }

/* ---------- lists of names, shader sources ---------- */

static void gl_x_call_lists(Cpu *c, GlCx *x, uint32_t a)
{
    uint32_t n = GI(0), type = GI(4), p = GI(8), i;
    void *t = (void *)(uintptr_t)p;
    if (type == 0x1402 || type == 0x1403) {                  /* short */
        uint16_t *q = gl_tmp(2, n, 2);
        if (!q) return;
        for (i = 0; i < n; i++) q[i] = (uint16_t)ld16(p + 2 * i);
        t = q;
    } else if (type == 0x1404 || type == 0x1405 || type == 0x1406) {   /* int, float */
        uint32_t *q = gl_tmp(2, n, 4);
        if (!q) return;
        for (i = 0; i < n; i++) q[i] = ld32(p + 4 * i);
        t = q;
    }                                           /* bytes and GL_2/3/4_BYTES: byte sequences as they are */
    HV(S.call_lists, n, type, t);
    (void)c;
}

static void gl_x_shader_source_ARB(Cpu *c, GlCx *x, uint32_t a)
{
    uint32_t n = GI(4), sp = GI(8), lp = GI(12), i;
    const char **str = gl_tmp(2, n, sizeof(char *));
    int32_t *len = lp ? gl_tmp(3, n, 4) : 0;
    if (!str || (lp && !len)) return;
    for (i = 0; i < n; i++) {
        str[i] = (const char *)(uintptr_t)ld32(sp + 4 * i);
        if (len) len[i] = (int32_t)ld32(lp + 4 * i);
    }
    HV(S.shader_source_ARB, GI(0), n, str, len);
    (void)c;
}
#undef ST
