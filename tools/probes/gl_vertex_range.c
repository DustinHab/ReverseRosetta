/* gl_vertex_range.c: native, client arrays rewritten every frame into a vertex array range (APPLE_vertex_array_range,
   the chip reads the memory itself) against plain client arrays
   Build: gcc-4.2 -O2 -o /tmp/gl_vertex_range tools/probes/gl_vertex_range.c -framework OpenGL; run it in the login session (tests/in_session.sh). */
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <mach/mach_time.h>
#include <sys/time.h>

#define W 256
#define H 256
#define NV 18000
#define REG (1 << 20)
#define NREG 3
static float pos[NV * 3], nrm[NV * 3];
static unsigned char col[NV * 4];
struct vtx { float p[3], n[3]; unsigned char c[4]; };
static struct vtx inter[NV];
static char *var;
static GLuint fence[NREG];
static int vk, fence_set[NREG];
static double cpy, waitt;
static double ms(uint64_t d) { mach_timebase_info_data_t tb; mach_timebase_info(&tb); return (double)d * tb.numer / tb.denom / 1e6; }
static double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec / 1e6; }

static double frame(int mode)
{
    uint64_t t0;
    double d;
    char *m = 0;
    size_t len = 0;
    glClearColor(0.1f, 0.1f, 0.2f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    if (mode >= 1) {
        vk = (vk + 1) % NREG;
        m = var + vk * REG;
        t0 = mach_absolute_time();
        if (fence_set[vk]) glFinishFenceAPPLE(fence[vk]);
        waitt += ms(mach_absolute_time() - t0);
    }
    t0 = mach_absolute_time();
    if (mode == 1 || mode == 2) {
        memcpy(m, pos, sizeof pos); memcpy(m + sizeof pos, nrm, sizeof nrm); memcpy(m + 2 * sizeof pos, col, sizeof col);
        len = 2 * sizeof pos + sizeof col;
    } else if (mode == 3) {
        memcpy(m, inter, sizeof inter);
        len = sizeof inter;
    }
    if (m) glFlushVertexArrayRangeAPPLE(len, m);
    cpy += ms(mach_absolute_time() - t0);
    if (mode == 0) {
        glVertexPointer(3, GL_FLOAT, 0, pos); glNormalPointer(GL_FLOAT, 0, nrm); glColorPointer(4, GL_UNSIGNED_BYTE, 0, col);
    } else if (mode < 3) {
        glVertexPointer(3, GL_FLOAT, 0, m); glNormalPointer(GL_FLOAT, 0, m + sizeof pos); glColorPointer(4, GL_UNSIGNED_BYTE, 0, m + 2 * sizeof pos);
    } else {
        struct vtx *v = (struct vtx *)m;
        glVertexPointer(3, GL_FLOAT, 28, v[0].p); glNormalPointer(GL_FLOAT, 28, v[0].n); glColorPointer(4, GL_UNSIGNED_BYTE, 28, v[0].c);
    }
    t0 = mach_absolute_time();
    glDrawArrays(GL_TRIANGLES, 0, NV);
    d = ms(mach_absolute_time() - t0);
    if (m) { glSetFenceAPPLE(fence[vk]); fence_set[vk] = 1; }
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    return d;
}

int main(int argc, char **argv)
{
    static const char *name[] = { "client arrays (no copy)", "VAR shared, rewritten", "VAR cached, rewritten", "VAR shared, interleaved" };
    CGLPixelFormatAttribute at[] = { kCGLPFAAccelerated, kCGLPFAPBuffer, kCGLPFAColorSize, 24, kCGLPFADepthSize, 16, 0 };
    CGLPixelFormatObj pf;
    CGLContextObj ctx;
    CGLPBufferObj pb;
    GLint n, vs = 0;
    int i, mode;
    CGLChoosePixelFormat(at, &pf, &n);
    CGLCreateContext(pf, 0, &ctx);
    CGLCreatePBuffer(W, H, GL_TEXTURE_2D, GL_RGBA, 0, &pb);
    CGLGetVirtualScreen(ctx, &vs);
    CGLSetPBuffer(ctx, pb, 0, 0, vs);
    CGLSetCurrentContext(ctx);
    printf("VAR %s, fence %s\n", strstr((const char *)glGetString(GL_EXTENSIONS), "GL_APPLE_vertex_array_range") ? "yes" : "no",
           strstr((const char *)glGetString(GL_EXTENSIONS), "GL_APPLE_fence") ? "yes" : "no");
    for (i = 0; i < NV; i++) {
        float a = i * 0.013f, r = 0.2f + 0.7f * (float)((i * 7919) % 1000) / 1000.0f;
        pos[3 * i] = r * cosf(a); pos[3 * i + 1] = r * sinf(a * 1.7f); pos[3 * i + 2] = -0.5f + (i % 100) / 100.0f;
        nrm[3 * i] = cosf(a); nrm[3 * i + 1] = sinf(a); nrm[3 * i + 2] = 0.3f;
        col[4 * i] = i; col[4 * i + 1] = i >> 3; col[4 * i + 2] = i >> 6; col[4 * i + 3] = 255;
        memcpy(inter[i].p, pos + 3 * i, 12); memcpy(inter[i].n, nrm + 3 * i, 12); memcpy(inter[i].c, col + 4 * i, 4);
    }
    var = valloc(NREG * REG);
    memset(var, 0, NREG * REG);
    glGenFencesAPPLE(NREG, fence);
    glViewport(0, 0, W, H);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHT0);
    glEnable(GL_COLOR_MATERIAL);
    for (mode = 0; mode < 4; mode++) {
        double best = 1e9, draw = 0;
        int r, f;
        if (mode == 1 || mode == 2) {
            glVertexArrayParameteriAPPLE(GL_VERTEX_ARRAY_STORAGE_HINT_APPLE, mode == 1 ? GL_STORAGE_SHARED_APPLE : GL_STORAGE_CACHED_APPLE);
            glVertexArrayRangeAPPLE(NREG * REG, var);
            glEnableClientState(GL_VERTEX_ARRAY_RANGE_APPLE);
        }
        if (mode == 3) {
            glDisableClientState(GL_VERTEX_ARRAY_RANGE_APPLE);
            glVertexArrayParameteriAPPLE(GL_VERTEX_ARRAY_STORAGE_HINT_APPLE, GL_STORAGE_SHARED_APPLE);
            glVertexArrayRangeAPPLE(NREG * REG, var);
            glEnableClientState(GL_VERTEX_ARRAY_RANGE_APPLE);
        }
        cpy = waitt = 0;
        for (r = 0; r < 6; r++) {
            double t0 = now(), t;
            for (f = 0; f < 4; f++) { double d = frame(mode); if (r) draw += d; }
            glFinish();
            t = (now() - t0) / 4;
            if (r && t < best) best = t;
        }
        printf("%-30s frame %6.3f ms  copy %6.3f ms  wait %6.3f ms  glDrawArrays %6.3f ms\n", name[mode], best * 1e3, cpy / 24, waitt / 24, draw / 20);
    }
    printf("gl error %x\n", glGetError());
    return 0;
}
