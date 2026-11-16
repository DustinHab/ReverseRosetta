/* al.c - OpenAL for Intel programs: the system's PowerPC OpenAL (OpenAL.framework, CoreAudio) instead of the i386
 * one, which the 10.5.8 update does not contain (Intel root: a stub library with the symbols of the SDK). thunk.c
 * replaces the exports of the guest's OpenAL image by name with the functions here (al_stub).
 *
 * Arguments come from the Intel stack (32-bit words in Intel byte order, a double takes two). Handles (ALCdevice *,
 * ALCcontext *) and strings are host pointers in the shared address space and pass as they are; source and buffer
 * names are integers. What the host writes into the program's memory is swapped: arrays of names, values of
 * getters, captured samples. Sample data of alBufferData in 16-bit and 32-bit float formats is swapped into a copy
 * (the host copies it again); alBufferDataStatic keeps the swapped copy until the buffer is deleted or filled again.
 * Floating-point results go to st(0) as in the i386 calling convention. */
#include "al.h"
#include "cpu.h"
#include "thunk.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define A(k) ld32(c->r[ESP] + 4 + 4 * (k))
#define AP(k) ((void *)(uintptr_t)A(k))
#define AF(k) al_f(A(k))
#define AD(k) al_d(ld32(c->r[ESP] + 4 + 4 * (k)), ld32(c->r[ESP] + 8 + 4 * (k)))
#define RET(v) do { c->r[EAX] = (uint32_t)(v); al_log(c, __func__); return 0; } while (0)
#define DONE() do { al_log(c, __func__); return 0; } while (0)

static float al_f(uint32_t w) { union { uint32_t w; float f; } u; u.w = w; return u.f; }
static uint32_t al_w(float f) { union { uint32_t w; float f; } u; u.f = f; return u.w; }
static double al_d(uint32_t lo, uint32_t hi) { union { uint64_t q; double d; } u; u.q = (uint64_t)hi << 32 | lo; return u.d; }

/* float or double result: st(0) */
static void al_st0(Cpu *c, double v)
{
    c->st_top = (c->st_top - 1) & 7;
    c->st[c->st_top] = v;
    if (c->x87_depth < 8) c->x87_depth++;
}

/* ROSETTA_AL_LOG=1: every call with its first arguments */
static int g_al_log = -1;
static void al_log(Cpu *c, const char *f)
{
    if (g_al_log < 0) g_al_log = getenv("ROSETTA_AL_LOG") != 0;
    if (g_al_log) fprintf(stderr, "[al] %s(%08x %08x %08x %08x) = %08x\n", f + 2, A(0), A(1), A(2), A(3), c->r[EAX]);
}

/* the host's function by name (the framework is loaded at the first call) */
static void *g_al;
static void *al_sym(const char *n)
{
    void *f;
    if (!g_al && !(g_al = dlopen("/System/Library/Frameworks/OpenAL.framework/OpenAL", RTLD_LAZY | RTLD_LOCAL))) {
        fprintf(stderr, "rosetta: OpenAL: %s\n", dlerror());
        return 0;
    }
    if (!(f = dlsym(g_al, n))) fprintf(stderr, "rosetta: OpenAL: no %s on this system\n", n);
    return f;
}
static void *al_set(void **p, const char *n) { return *p = al_sym(n); }
#define SYM(n) (f_##n ? f_##n : al_set(&f_##n, #n))
#define F(n) static void *f_##n
F(alEnable); F(alDisable); F(alIsEnabled); F(alGetString); F(alGetBooleanv); F(alGetIntegerv); F(alGetFloatv);
F(alGetDoublev); F(alGetBoolean); F(alGetInteger); F(alGetFloat); F(alGetDouble); F(alGetError);
F(alIsExtensionPresent); F(alGetEnumValue); F(alListenerf); F(alListener3f); F(alListenerfv); F(alListeneri);
F(alListener3i); F(alListeneriv); F(alGetListenerf); F(alGetListener3f); F(alGetListenerfv); F(alGetListeneri);
F(alGetListener3i); F(alGetListeneriv); F(alGenSources); F(alDeleteSources); F(alIsSource); F(alSourcef);
F(alSource3f); F(alSourcefv); F(alSourcei); F(alSource3i); F(alSourceiv); F(alGetSourcef); F(alGetSource3f);
F(alGetSourcefv); F(alGetSourcei); F(alGetSource3i); F(alGetSourceiv); F(alSourcePlayv); F(alSourceStopv);
F(alSourceRewindv); F(alSourcePausev); F(alSourcePlay); F(alSourceStop); F(alSourceRewind); F(alSourcePause);
F(alSourceQueueBuffers); F(alSourceUnqueueBuffers); F(alGenBuffers); F(alDeleteBuffers); F(alIsBuffer);
F(alBufferData); F(alBufferf); F(alBuffer3f); F(alBufferfv); F(alBufferi); F(alBuffer3i); F(alBufferiv);
F(alGetBufferf); F(alGetBuffer3f); F(alGetBufferfv); F(alGetBufferi); F(alGetBuffer3i); F(alGetBufferiv);
F(alDopplerFactor); F(alDopplerVelocity); F(alSpeedOfSound); F(alDistanceModel); F(alBufferDataStatic);
F(alcCreateContext); F(alcMakeContextCurrent); F(alcProcessContext); F(alcSuspendContext); F(alcDestroyContext);
F(alcGetCurrentContext); F(alcGetContextsDevice); F(alcOpenDevice); F(alcCloseDevice); F(alcGetError);
F(alcIsExtensionPresent); F(alcGetEnumValue); F(alcGetString); F(alcGetIntegerv); F(alcCaptureOpenDevice);
F(alcCaptureCloseDevice); F(alcCaptureStart); F(alcCaptureStop); F(alcCaptureSamples);
F(alcMacOSXMixerOutputRate); F(alcMacOSXGetMixerOutputRate); F(alcMacOSXRenderingQuality);
F(alcMacOSXGetRenderingQuality); F(alcMacOSXMixerMaxiumumBusses); F(alcMacOSXGetMixerMaxiumumBusses);
#undef F

/* scratch for arrays (one guest thread at a time) */
static uint8_t *g_tmp;
static uint32_t g_tmp_size;
static void *tmp(uint32_t n)
{
    if (n > (1u << 28)) return 0;
    if (n > g_tmp_size) {
        void *p = realloc(g_tmp, n ? n : 1);
        if (!p) return 0;
        g_tmp = p;
        g_tmp_size = n;
    }
    return g_tmp;
}
static uint32_t *in_words(uint32_t a, uint32_t n)
{
    uint32_t *t = tmp(4 * n), i;
    if (t && a) for (i = 0; i < n; i++) t[i] = ld32(a + 4 * i);
    return t;
}
static void out_words(uint32_t a, const uint32_t *t, uint32_t n)
{
    uint32_t i;
    if (a && t) for (i = 0; i < n; i++) st32(a + 4 * i, t[i]);
}

/* values of a listener/source/buffer vector parameter: 3 for positions, velocities and directions, 6 for the
   orientation, otherwise 1 */
static uint32_t al_count(uint32_t p)
{
    switch (p) {
    case 0x1004: case 0x1005: case 0x1006: return 3;      /* AL_POSITION, AL_DIRECTION, AL_VELOCITY */
    case 0x100F: return 6;                                  /* AL_ORIENTATION */
    }
    return 1;
}

/* ---------- state ---------- */

static int c_alEnable(Cpu *c) { ((void (*)(int))SYM(alEnable))((int)A(0)); DONE(); }
static int c_alDisable(Cpu *c) { ((void (*)(int))SYM(alDisable))((int)A(0)); DONE(); }
static int c_alIsEnabled(Cpu *c) { RET(((unsigned char (*)(int))SYM(alIsEnabled))((int)A(0))); }
static int c_alGetString(Cpu *c) { RET((uintptr_t)((const char *(*)(int))SYM(alGetString))((int)A(0))); }
static int c_alGetError(Cpu *c) { RET(((int (*)(void))SYM(alGetError))()); }
static int c_alIsExtensionPresent(Cpu *c) { RET(((unsigned char (*)(const char *))SYM(alIsExtensionPresent))(AP(0))); }
static int c_alGetEnumValue(Cpu *c) { RET(((int (*)(const char *))SYM(alGetEnumValue))(AP(0))); }
static int c_alGetBoolean(Cpu *c) { RET(((unsigned char (*)(int))SYM(alGetBoolean))((int)A(0))); }
static int c_alGetInteger(Cpu *c) { RET(((int (*)(int))SYM(alGetInteger))((int)A(0))); }
static int c_alGetFloat(Cpu *c) { al_st0(c, ((float (*)(int))SYM(alGetFloat))((int)A(0))); DONE(); }
static int c_alGetDouble(Cpu *c) { al_st0(c, ((double (*)(int))SYM(alGetDouble))((int)A(0))); DONE(); }
static int c_alGetBooleanv(Cpu *c)
{
    unsigned char v[16] = { 0 };
    ((void (*)(int, unsigned char *))SYM(alGetBooleanv))((int)A(0), v);
    if (A(1)) st8(A(1), v[0]);
    DONE();
}
static int c_alGetIntegerv(Cpu *c)
{
    int v[16] = { 0 };
    ((void (*)(int, int *))SYM(alGetIntegerv))((int)A(0), v);
    out_words(A(1), (uint32_t *)v, 1);
    DONE();
}
static int c_alGetFloatv(Cpu *c)
{
    float v[16] = { 0 };
    ((void (*)(int, float *))SYM(alGetFloatv))((int)A(0), v);
    if (A(1)) st32(A(1), al_w(v[0]));
    DONE();
}
static int c_alGetDoublev(Cpu *c)
{
    union { double d[16]; uint64_t q[16]; } v;
    memset(&v, 0, sizeof v);
    ((void (*)(int, double *))SYM(alGetDoublev))((int)A(0), v.d);
    if (A(1)) st64(A(1), v.q[0]);
    DONE();
}
static int c_alDopplerFactor(Cpu *c) { ((void (*)(float))SYM(alDopplerFactor))(AF(0)); DONE(); }
static int c_alDopplerVelocity(Cpu *c) { ((void (*)(float))SYM(alDopplerVelocity))(AF(0)); DONE(); }
static int c_alSpeedOfSound(Cpu *c) { ((void (*)(float))SYM(alSpeedOfSound))(AF(0)); DONE(); }
static int c_alDistanceModel(Cpu *c) { ((void (*)(int))SYM(alDistanceModel))((int)A(0)); DONE(); }

/* ---------- listener ---------- */

static int c_alListenerf(Cpu *c) { ((void (*)(int, float))SYM(alListenerf))((int)A(0), AF(1)); DONE(); }
static int c_alListener3f(Cpu *c) { ((void (*)(int, float, float, float))SYM(alListener3f))((int)A(0), AF(1), AF(2), AF(3)); DONE(); }
static int c_alListeneri(Cpu *c) { ((void (*)(int, int))SYM(alListeneri))((int)A(0), (int)A(1)); DONE(); }
static int c_alListener3i(Cpu *c) { ((void (*)(int, int, int, int))SYM(alListener3i))((int)A(0), (int)A(1), (int)A(2), (int)A(3)); DONE(); }
static int c_alListenerfv(Cpu *c)
{
    uint32_t *t = in_words(A(1), al_count(A(0)));
    ((void (*)(int, const void *))SYM(alListenerfv))((int)A(0), A(1) ? t : 0);
    DONE();
}
static int c_alListeneriv(Cpu *c)
{
    uint32_t *t = in_words(A(1), al_count(A(0)));
    ((void (*)(int, const void *))SYM(alListeneriv))((int)A(0), A(1) ? t : 0);
    DONE();
}
static int c_alGetListenerf(Cpu *c)
{
    float v = 0;
    ((void (*)(int, float *))SYM(alGetListenerf))((int)A(0), &v);
    if (A(1)) st32(A(1), al_w(v));
    DONE();
}
static int c_alGetListeneri(Cpu *c)
{
    int v = 0;
    ((void (*)(int, int *))SYM(alGetListeneri))((int)A(0), &v);
    if (A(1)) st32(A(1), (uint32_t)v);
    DONE();
}
static int c_alGetListener3f(Cpu *c)
{
    float v[3] = { 0, 0, 0 };
    ((void (*)(int, float *, float *, float *))SYM(alGetListener3f))((int)A(0), &v[0], &v[1], &v[2]);
    if (A(1)) st32(A(1), al_w(v[0]));
    if (A(2)) st32(A(2), al_w(v[1]));
    if (A(3)) st32(A(3), al_w(v[2]));
    DONE();
}
static int c_alGetListener3i(Cpu *c)
{
    int v[3] = { 0, 0, 0 };
    ((void (*)(int, int *, int *, int *))SYM(alGetListener3i))((int)A(0), &v[0], &v[1], &v[2]);
    if (A(1)) st32(A(1), (uint32_t)v[0]);
    if (A(2)) st32(A(2), (uint32_t)v[1]);
    if (A(3)) st32(A(3), (uint32_t)v[2]);
    DONE();
}
static int c_alGetListenerfv(Cpu *c)
{
    uint32_t v[6] = { 0 };
    ((void (*)(int, void *))SYM(alGetListenerfv))((int)A(0), v);
    out_words(A(1), v, al_count(A(0)));
    DONE();
}
static int c_alGetListeneriv(Cpu *c)
{
    uint32_t v[6] = { 0 };
    ((void (*)(int, void *))SYM(alGetListeneriv))((int)A(0), v);
    out_words(A(1), v, al_count(A(0)));
    DONE();
}

/* ---------- sources ---------- */

static int c_alGenSources(Cpu *c)
{
    uint32_t n = A(0), *t = tmp(4 * n);
    if (!t) DONE();
    ((void (*)(int, uint32_t *))SYM(alGenSources))((int)n, t);
    out_words(A(1), t, n);
    DONE();
}
static int c_alDeleteSources(Cpu *c)
{
    uint32_t n = A(0), *t = in_words(A(1), n);
    if (t) ((void (*)(int, const uint32_t *))SYM(alDeleteSources))((int)n, t);
    DONE();
}
static int c_alIsSource(Cpu *c) { RET(((unsigned char (*)(uint32_t))SYM(alIsSource))(A(0))); }
static int c_alSourcef(Cpu *c) { ((void (*)(uint32_t, int, float))SYM(alSourcef))(A(0), (int)A(1), AF(2)); DONE(); }
static int c_alSource3f(Cpu *c) { ((void (*)(uint32_t, int, float, float, float))SYM(alSource3f))(A(0), (int)A(1), AF(2), AF(3), AF(4)); DONE(); }
static int c_alSourcei(Cpu *c) { ((void (*)(uint32_t, int, int))SYM(alSourcei))(A(0), (int)A(1), (int)A(2)); DONE(); }
static int c_alSource3i(Cpu *c) { ((void (*)(uint32_t, int, int, int, int))SYM(alSource3i))(A(0), (int)A(1), (int)A(2), (int)A(3), (int)A(4)); DONE(); }
static int c_alSourcefv(Cpu *c)
{
    uint32_t *t = in_words(A(2), al_count(A(1)));
    ((void (*)(uint32_t, int, const void *))SYM(alSourcefv))(A(0), (int)A(1), A(2) ? t : 0);
    DONE();
}
static int c_alSourceiv(Cpu *c)
{
    uint32_t *t = in_words(A(2), al_count(A(1)));
    ((void (*)(uint32_t, int, const void *))SYM(alSourceiv))(A(0), (int)A(1), A(2) ? t : 0);
    DONE();
}
static int c_alGetSourcef(Cpu *c)
{
    float v = 0;
    ((void (*)(uint32_t, int, float *))SYM(alGetSourcef))(A(0), (int)A(1), &v);
    if (A(2)) st32(A(2), al_w(v));
    DONE();
}
static int c_alGetSourcei(Cpu *c)
{
    int v = 0;
    ((void (*)(uint32_t, int, int *))SYM(alGetSourcei))(A(0), (int)A(1), &v);
    if (A(2)) st32(A(2), (uint32_t)v);
    DONE();
}
static int c_alGetSource3f(Cpu *c)
{
    float v[3] = { 0, 0, 0 };
    ((void (*)(uint32_t, int, float *, float *, float *))SYM(alGetSource3f))(A(0), (int)A(1), &v[0], &v[1], &v[2]);
    if (A(2)) st32(A(2), al_w(v[0]));
    if (A(3)) st32(A(3), al_w(v[1]));
    if (A(4)) st32(A(4), al_w(v[2]));
    DONE();
}
static int c_alGetSource3i(Cpu *c)
{
    int v[3] = { 0, 0, 0 };
    ((void (*)(uint32_t, int, int *, int *, int *))SYM(alGetSource3i))(A(0), (int)A(1), &v[0], &v[1], &v[2]);
    if (A(2)) st32(A(2), (uint32_t)v[0]);
    if (A(3)) st32(A(3), (uint32_t)v[1]);
    if (A(4)) st32(A(4), (uint32_t)v[2]);
    DONE();
}
static int c_alGetSourcefv(Cpu *c)
{
    uint32_t v[6] = { 0 };
    ((void (*)(uint32_t, int, void *))SYM(alGetSourcefv))(A(0), (int)A(1), v);
    out_words(A(2), v, al_count(A(1)));
    DONE();
}
static int c_alGetSourceiv(Cpu *c)
{
    uint32_t v[6] = { 0 };
    ((void (*)(uint32_t, int, void *))SYM(alGetSourceiv))(A(0), (int)A(1), v);
    out_words(A(2), v, al_count(A(1)));
    DONE();
}
#define SRC_V(name)                                                                                              \
    static int c_##name(Cpu *c)                                                                                  \
    {                                                                                                            \
        uint32_t n = A(0), *t = in_words(A(1), n);                                                               \
        if (t) ((void (*)(int, const uint32_t *))SYM(name))((int)n, t);                                         \
        DONE();                                                                                                  \
    }
SRC_V(alSourcePlayv) SRC_V(alSourceStopv) SRC_V(alSourceRewindv) SRC_V(alSourcePausev)
#undef SRC_V
static int c_alSourcePlay(Cpu *c) { ((void (*)(uint32_t))SYM(alSourcePlay))(A(0)); DONE(); }
static int c_alSourceStop(Cpu *c) { ((void (*)(uint32_t))SYM(alSourceStop))(A(0)); DONE(); }
static int c_alSourceRewind(Cpu *c) { ((void (*)(uint32_t))SYM(alSourceRewind))(A(0)); DONE(); }
static int c_alSourcePause(Cpu *c) { ((void (*)(uint32_t))SYM(alSourcePause))(A(0)); DONE(); }
static int c_alSourceQueueBuffers(Cpu *c)
{
    uint32_t n = A(1), *t = in_words(A(2), n);
    if (t) ((void (*)(uint32_t, int, const uint32_t *))SYM(alSourceQueueBuffers))(A(0), (int)n, t);
    DONE();
}
static int c_alSourceUnqueueBuffers(Cpu *c)
{
    uint32_t n = A(1), *t = tmp(4 * n);
    if (!t) DONE();
    memset(t, 0, 4 * n);
    ((void (*)(uint32_t, int, uint32_t *))SYM(alSourceUnqueueBuffers))(A(0), (int)n, t);
    out_words(A(2), t, n);
    DONE();
}

/* ---------- buffers ---------- */

/* sample size to swap for a format: 2 (16-bit integer), 4 (32-bit float, Apple's extension), 0 (bytes) */
static int sample_swap(uint32_t format)
{
    switch (format) {
    case 0x1101: case 0x1103: return 2;                    /* AL_FORMAT_MONO16, AL_FORMAT_STEREO16 */
    case 0x10010: case 0x10011: return 4;                  /* AL_FORMAT_MONO_FLOAT32, AL_FORMAT_STEREO_FLOAT32 */
    }
    return 0;
}

/* data in host byte order: the program's bytes (8-bit samples), or a copy into t (n bytes) */
static void *samples_host(uint32_t data, uint32_t n, int sz, void *t)
{
    uint32_t i;
    if (!sz || !data) return (void *)(uintptr_t)data;
    if (sz == 2) for (i = 0; i + 2 <= n; i += 2) ((uint16_t *)t)[i / 2] = (uint16_t)ld16(data + i);
    else for (i = 0; i + 4 <= n; i += 4) ((uint32_t *)t)[i / 4] = ld32(data + i);
    return t;
}

static int c_alGenBuffers(Cpu *c)
{
    uint32_t n = A(0), *t = tmp(4 * n);
    if (!t) DONE();
    ((void (*)(int, uint32_t *))SYM(alGenBuffers))((int)n, t);
    out_words(A(1), t, n);
    DONE();
}

/* swapped copies that alBufferDataStatic hands to the host (it reads them as long as the buffer has them) */
#define STATIC_N 1024
static struct { uint32_t name; void *copy; } g_static[STATIC_N];
static void static_drop(uint32_t name)
{
    int k;
    for (k = 0; k < STATIC_N; k++) if (g_static[k].copy && g_static[k].name == name) { free(g_static[k].copy); g_static[k].copy = 0; }
}
static int c_alDeleteBuffers(Cpu *c)
{
    uint32_t n = A(0), *t = in_words(A(1), n), i;
    if (!t) DONE();
    ((void (*)(int, const uint32_t *))SYM(alDeleteBuffers))((int)n, t);
    for (i = 0; i < n; i++)                 /* (alGetError would take the program's error) */
        if (!((unsigned char (*)(uint32_t))SYM(alIsBuffer))(t[i])) static_drop(t[i]);
    DONE();
}
static int c_alIsBuffer(Cpu *c) { RET(((unsigned char (*)(uint32_t))SYM(alIsBuffer))(A(0))); }
static int c_alBufferData(Cpu *c)
{
    uint32_t n = A(3);
    int sz = sample_swap(A(1));
    void *t = sz ? tmp(n) : 0, *d = sz && !t ? 0 : samples_host(A(2), n, sz, t);
    ((void (*)(uint32_t, int, const void *, int, int))SYM(alBufferData))(A(0), (int)A(1), d, (int)n, (int)A(4));
    static_drop(A(0));
    DONE();
}
static int c_alBufferDataStatic(Cpu *c)
{
    uint32_t n = A(3);
    int sz = sample_swap(A(1)), k;
    void *d = (void *)(uintptr_t)A(2);
    static_drop(A(0));
    if (sz && A(2)) {
        void *t = malloc(n ? n : 1);
        if (!t) DONE();
        d = samples_host(A(2), n, sz, t);
        for (k = 0; k < STATIC_N && g_static[k].copy; k++) ;
        if (k < STATIC_N) { g_static[k].name = A(0); g_static[k].copy = t; }
    }
    ((void (*)(int, int, const void *, int, int))SYM(alBufferDataStatic))((int)A(0), (int)A(1), d, (int)n, (int)A(4));
    DONE();
}
static int c_alBufferf(Cpu *c) { ((void (*)(uint32_t, int, float))SYM(alBufferf))(A(0), (int)A(1), AF(2)); DONE(); }
static int c_alBuffer3f(Cpu *c) { ((void (*)(uint32_t, int, float, float, float))SYM(alBuffer3f))(A(0), (int)A(1), AF(2), AF(3), AF(4)); DONE(); }
static int c_alBufferi(Cpu *c) { ((void (*)(uint32_t, int, int))SYM(alBufferi))(A(0), (int)A(1), (int)A(2)); DONE(); }
static int c_alBuffer3i(Cpu *c) { ((void (*)(uint32_t, int, int, int, int))SYM(alBuffer3i))(A(0), (int)A(1), (int)A(2), (int)A(3), (int)A(4)); DONE(); }
static int c_alBufferfv(Cpu *c)
{
    uint32_t *t = in_words(A(2), al_count(A(1)));
    ((void (*)(uint32_t, int, const void *))SYM(alBufferfv))(A(0), (int)A(1), A(2) ? t : 0);
    DONE();
}
static int c_alBufferiv(Cpu *c)
{
    uint32_t *t = in_words(A(2), al_count(A(1)));
    ((void (*)(uint32_t, int, const void *))SYM(alBufferiv))(A(0), (int)A(1), A(2) ? t : 0);
    DONE();
}
static int c_alGetBufferf(Cpu *c)
{
    float v = 0;
    ((void (*)(uint32_t, int, float *))SYM(alGetBufferf))(A(0), (int)A(1), &v);
    if (A(2)) st32(A(2), al_w(v));
    DONE();
}
static int c_alGetBufferi(Cpu *c)
{
    int v = 0;
    ((void (*)(uint32_t, int, int *))SYM(alGetBufferi))(A(0), (int)A(1), &v);
    if (A(2)) st32(A(2), (uint32_t)v);
    DONE();
}
static int c_alGetBuffer3f(Cpu *c)
{
    float v[3] = { 0, 0, 0 };
    ((void (*)(uint32_t, int, float *, float *, float *))SYM(alGetBuffer3f))(A(0), (int)A(1), &v[0], &v[1], &v[2]);
    if (A(2)) st32(A(2), al_w(v[0]));
    if (A(3)) st32(A(3), al_w(v[1]));
    if (A(4)) st32(A(4), al_w(v[2]));
    DONE();
}
static int c_alGetBuffer3i(Cpu *c)
{
    int v[3] = { 0, 0, 0 };
    ((void (*)(uint32_t, int, int *, int *, int *))SYM(alGetBuffer3i))(A(0), (int)A(1), &v[0], &v[1], &v[2]);
    if (A(2)) st32(A(2), (uint32_t)v[0]);
    if (A(3)) st32(A(3), (uint32_t)v[1]);
    if (A(4)) st32(A(4), (uint32_t)v[2]);
    DONE();
}
static int c_alGetBufferfv(Cpu *c)
{
    uint32_t v[6] = { 0 };
    ((void (*)(uint32_t, int, void *))SYM(alGetBufferfv))(A(0), (int)A(1), v);
    out_words(A(2), v, al_count(A(1)));
    DONE();
}
static int c_alGetBufferiv(Cpu *c)
{
    uint32_t v[6] = { 0 };
    ((void (*)(uint32_t, int, void *))SYM(alGetBufferiv))(A(0), (int)A(1), v);
    out_words(A(2), v, al_count(A(1)));
    DONE();
}

/* ---------- contexts and devices (ALC) ---------- */

static int c_alcOpenDevice(Cpu *c) { RET((uintptr_t)((void *(*)(const char *))SYM(alcOpenDevice))(AP(0))); }
static int c_alcCloseDevice(Cpu *c) { RET(((unsigned char (*)(void *))SYM(alcCloseDevice))(AP(0))); }
static int c_alcCreateContext(Cpu *c)
{
    uint32_t a = A(1), n = 0, *t;
    if (a) while (n < 256 && ld32(a + 4 * n)) n += 2;       /* key/value pairs up to the 0 */
    t = in_words(a, n + 1);
    if (t) t[n] = 0;
    RET((uintptr_t)((void *(*)(void *, const uint32_t *))SYM(alcCreateContext))(AP(0), a ? t : 0));
}
static int c_alcMakeContextCurrent(Cpu *c) { RET(((unsigned char (*)(void *))SYM(alcMakeContextCurrent))(AP(0))); }
static int c_alcProcessContext(Cpu *c) { ((void (*)(void *))SYM(alcProcessContext))(AP(0)); DONE(); }
static int c_alcSuspendContext(Cpu *c) { ((void (*)(void *))SYM(alcSuspendContext))(AP(0)); DONE(); }
static int c_alcDestroyContext(Cpu *c) { ((void (*)(void *))SYM(alcDestroyContext))(AP(0)); DONE(); }
static int c_alcGetCurrentContext(Cpu *c) { RET((uintptr_t)((void *(*)(void))SYM(alcGetCurrentContext))()); }
static int c_alcGetContextsDevice(Cpu *c) { RET((uintptr_t)((void *(*)(void *))SYM(alcGetContextsDevice))(AP(0))); }
static int c_alcGetError(Cpu *c) { RET(((int (*)(void *))SYM(alcGetError))(AP(0))); }
static int c_alcIsExtensionPresent(Cpu *c) { RET(((unsigned char (*)(void *, const char *))SYM(alcIsExtensionPresent))(AP(0), AP(1))); }
static int c_alcGetEnumValue(Cpu *c) { RET(((int (*)(void *, const char *))SYM(alcGetEnumValue))(AP(0), AP(1))); }
static int c_alcGetString(Cpu *c) { RET((uintptr_t)((const char *(*)(void *, int))SYM(alcGetString))(AP(0), (int)A(1))); }
static int c_alcGetIntegerv(Cpu *c)
{
    uint32_t n = A(2), *t = tmp(4 * (n ? n : 1));
    if (!t || n > (1u << 20)) DONE();
    memset(t, 0, 4 * (n ? n : 1));
    ((void (*)(void *, int, int, uint32_t *))SYM(alcGetIntegerv))(AP(0), (int)A(1), (int)n, A(3) ? t : 0);
    out_words(A(3), t, n);
    DONE();
}

/* capture devices: the sample format of each (to swap what alcCaptureSamples writes) */
#define CAPTURE_N 16
static struct { uint32_t dev; uint32_t format; } g_capture[CAPTURE_N];
static int c_alcCaptureOpenDevice(Cpu *c)
{
    void *d = ((void *(*)(const char *, uint32_t, int, int))SYM(alcCaptureOpenDevice))(AP(0), A(1), (int)A(2), (int)A(3));
    int k;
    if (d) for (k = 0; k < CAPTURE_N; k++) if (!g_capture[k].dev) { g_capture[k].dev = (uint32_t)(uintptr_t)d; g_capture[k].format = A(2); break; }
    RET((uintptr_t)d);
}
static int c_alcCaptureCloseDevice(Cpu *c)
{
    int k;
    for (k = 0; k < CAPTURE_N; k++) if (g_capture[k].dev == A(0)) g_capture[k].dev = 0;
    RET(((unsigned char (*)(void *))SYM(alcCaptureCloseDevice))(AP(0)));
}
static int c_alcCaptureStart(Cpu *c) { ((void (*)(void *))SYM(alcCaptureStart))(AP(0)); DONE(); }
static int c_alcCaptureStop(Cpu *c) { ((void (*)(void *))SYM(alcCaptureStop))(AP(0)); DONE(); }
static int c_alcCaptureSamples(Cpu *c)
{
    uint32_t format = 0, frame, n, i, a = A(1);
    int k, sz;
    for (k = 0; k < CAPTURE_N; k++) if (g_capture[k].dev == A(0)) format = g_capture[k].format;
    ((void (*)(void *, void *, int))SYM(alcCaptureSamples))(AP(0), AP(1), (int)A(2));
    sz = sample_swap(format);
    frame = (format == 0x1103 || format == 0x1102 || format == 0x10011) ? 2u : 1u;
    n = A(2) * frame;
    if (sz == 2 && a) for (i = 0; i < n; i++) st16(a + 2 * i, *(uint16_t *)(uintptr_t)(a + 2 * i));
    if (sz == 4 && a) for (i = 0; i < n; i++) st32(a + 4 * i, *(uint32_t *)(uintptr_t)(a + 4 * i));
    DONE();
}

/* Apple's extensions (MacOSX_OALExtensions.h) */
static int c_alcMacOSXMixerOutputRate(Cpu *c) { ((void (*)(double))SYM(alcMacOSXMixerOutputRate))(AD(0)); DONE(); }
static int c_alcMacOSXGetMixerOutputRate(Cpu *c) { al_st0(c, ((double (*)(void))SYM(alcMacOSXGetMixerOutputRate))()); DONE(); }
static int c_alcMacOSXRenderingQuality(Cpu *c) { ((void (*)(int))SYM(alcMacOSXRenderingQuality))((int)A(0)); DONE(); }
static int c_alcMacOSXGetRenderingQuality(Cpu *c) { RET(((int (*)(void))SYM(alcMacOSXGetRenderingQuality))()); }
static int c_alcMacOSXMixerMaxiumumBusses(Cpu *c) { ((void (*)(int))SYM(alcMacOSXMixerMaxiumumBusses))((int)A(0)); DONE(); }
static int c_alcMacOSXGetMixerMaxiumumBusses(Cpu *c) { RET(((int (*)(void))SYM(alcMacOSXGetMixerMaxiumumBusses))()); }

/* ---------- names ---------- */

static int c_alGetProcAddress(Cpu *c);
static int c_alcGetProcAddress(Cpu *c);
#define E(n) { #n, c_##n }
static const struct { const char *name; Stub fn; } g_al_list[] = {
    E(alEnable), E(alDisable), E(alIsEnabled), E(alGetString), E(alGetBooleanv), E(alGetIntegerv), E(alGetFloatv),
    E(alGetDoublev), E(alGetBoolean), E(alGetInteger), E(alGetFloat), E(alGetDouble), E(alGetError),
    E(alIsExtensionPresent), E(alGetProcAddress), E(alGetEnumValue), E(alListenerf), E(alListener3f), E(alListenerfv),
    E(alListeneri), E(alListener3i), E(alListeneriv), E(alGetListenerf), E(alGetListener3f), E(alGetListenerfv),
    E(alGetListeneri), E(alGetListener3i), E(alGetListeneriv), E(alGenSources), E(alDeleteSources), E(alIsSource),
    E(alSourcef), E(alSource3f), E(alSourcefv), E(alSourcei), E(alSource3i), E(alSourceiv), E(alGetSourcef),
    E(alGetSource3f), E(alGetSourcefv), E(alGetSourcei), E(alGetSource3i), E(alGetSourceiv), E(alSourcePlayv),
    E(alSourceStopv), E(alSourceRewindv), E(alSourcePausev), E(alSourcePlay), E(alSourceStop), E(alSourceRewind),
    E(alSourcePause), E(alSourceQueueBuffers), E(alSourceUnqueueBuffers), E(alGenBuffers), E(alDeleteBuffers),
    E(alIsBuffer), E(alBufferData), E(alBufferf), E(alBuffer3f), E(alBufferfv), E(alBufferi), E(alBuffer3i),
    E(alBufferiv), E(alGetBufferf), E(alGetBuffer3f), E(alGetBufferfv), E(alGetBufferi), E(alGetBuffer3i),
    E(alGetBufferiv), E(alDopplerFactor), E(alDopplerVelocity), E(alSpeedOfSound), E(alDistanceModel),
    E(alBufferDataStatic), E(alcCreateContext), E(alcMakeContextCurrent), E(alcProcessContext),
    E(alcSuspendContext), E(alcDestroyContext), E(alcGetCurrentContext), E(alcGetContextsDevice), E(alcOpenDevice),
    E(alcCloseDevice), E(alcGetError), E(alcIsExtensionPresent), E(alcGetProcAddress), E(alcGetEnumValue),
    E(alcGetString), E(alcGetIntegerv), E(alcCaptureOpenDevice), E(alcCaptureCloseDevice), E(alcCaptureStart),
    E(alcCaptureStop), E(alcCaptureSamples), E(alcMacOSXMixerOutputRate), E(alcMacOSXGetMixerOutputRate),
    E(alcMacOSXRenderingQuality), E(alcMacOSXGetRenderingQuality), E(alcMacOSXMixerMaxiumumBusses),
    E(alcMacOSXGetMixerMaxiumumBusses),
};
#undef E
#define AL_N (sizeof g_al_list / sizeof g_al_list[0])

Stub al_stub(const char *s, size_t n)
{
    size_t k;
    if (n < 3 || s[0] != 'a' || s[1] != 'l') return 0;
    for (k = 0; k < AL_N; k++)
        if (strlen(g_al_list[k].name) == n && !memcmp(g_al_list[k].name, s, n)) return g_al_list[k].fn;
    return 0;
}

/* alGetProcAddress/alcGetProcAddress: the guest address of the export of the guest's OpenAL image, which thunk.c
   replaced by our function (thunk_guest_addr); 0 for what this file does not have */
static uint32_t proc_address(uint32_t name)
{
    char s[64];
    uint32_t i;
    Stub fn;
    if (!name) return 0;
    for (i = 0; i + 1 < sizeof s && ld8(name + i); i++) s[i] = (char)ld8(name + i);
    s[i] = 0;
    fn = al_stub(s, i);
    return fn ? thunk_guest_addr(fn) : 0;
}
static int c_alGetProcAddress(Cpu *c) { RET(proc_address(A(0))); }
static int c_alcGetProcAddress(Cpu *c) { RET(proc_address(A(1))); }
