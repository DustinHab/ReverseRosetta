/* gl_client_memory.c: native, client arrays from different memory: static, malloc, malloc rewritten before every draw
   (what a converted copy costs), page-aligned, double-buffered
   Build: gcc-4.2 -O2 -o /tmp/gl_client_memory tools/probes/gl_client_memory.c -framework OpenGL; run it in the login session (tests/in_session.sh). */
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
static float pos[NV * 3], nrm[NV * 3];
static unsigned char col[NV * 4];
struct vtx { float p[3], n[3]; unsigned char c[4]; };
static struct vtx inter[NV];
static char *buf[2], *pbuf[2];
static int bk;
static double cpy;
static double ms(uint64_t d) { mach_timebase_info_data_t tb; mach_timebase_info(&tb); return (double)d * tb.numer / tb.denom / 1e6; }
static double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec / 1e6; }

static double frame(int mode)
{
    uint64_t t0;
    double d;
    char *m = 0;
    glClearColor(0.1f, 0.1f, 0.2f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    t0 = mach_absolute_time();
    switch (mode) {
    case 0: m = 0; break;
    case 1: m = buf[0]; break;
    case 2: m = buf[0]; memcpy(m, pos, sizeof pos); memcpy(m + sizeof pos, nrm, sizeof nrm); break;
    case 3: bk ^= 1; m = buf[bk]; memcpy(m, pos, sizeof pos); memcpy(m + sizeof pos, nrm, sizeof nrm); break;
    case 4: m = pbuf[0]; memcpy(m, pos, sizeof pos); memcpy(m + sizeof pos, nrm, sizeof nrm); break;
    case 5: m = 0; break;
    case 6: m = buf[0]; memcpy(m, inter, sizeof inter); break;
    case 7: bk ^= 1; m = buf[bk]; memcpy(m, inter, sizeof inter); break;
    }
    cpy += ms(mach_absolute_time() - t0);
    if (mode < 5) {
        glVertexPointer(3, GL_FLOAT, 0, m ? (void *)m : (void *)pos);
        glNormalPointer(GL_FLOAT, 0, m ? (void *)(m + sizeof pos) : (void *)nrm);
        glColorPointer(4, GL_UNSIGNED_BYTE, 0, col);
    } else {
        struct vtx *v = m ? (struct vtx *)m : inter;
        glVertexPointer(3, GL_FLOAT, 28, v[0].p); glNormalPointer(GL_FLOAT, 28, v[0].n); glColorPointer(4, GL_UNSIGNED_BYTE, 28, v[0].c);
    }
    t0 = mach_absolute_time();
    glDrawArrays(GL_TRIANGLES, 0, NV);
    d = ms(mach_absolute_time() - t0);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    return d;
}

int main(void)
{
    static const char *name[] = { "client static", "client malloc", "client malloc rewritten", "rewritten, two buffers",
                                  "rewritten, page aligned", "interleaved static", "interleaved rewritten",
                                  "interleaved rewritten, two buffers" };
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
    for (i = 0; i < NV; i++) {
        float a = i * 0.013f;
        pos[3 * i] = 0.8f * cosf(a); pos[3 * i + 1] = 0.8f * sinf(a * 1.7f); pos[3 * i + 2] = -0.5f + (i % 100) / 100.0f;
        nrm[3 * i] = cosf(a); nrm[3 * i + 1] = sinf(a); nrm[3 * i + 2] = 0.3f;
        col[4 * i] = i; col[4 * i + 1] = i >> 3; col[4 * i + 2] = i >> 6; col[4 * i + 3] = 255;
        memcpy(inter[i].p, pos + 3 * i, 12); memcpy(inter[i].n, nrm + 3 * i, 12); memcpy(inter[i].c, col + 4 * i, 4);
    }
    for (i = 0; i < 2; i++) {
        buf[i] = malloc(sizeof inter + 64);
        pbuf[i] = valloc(sizeof inter);
        memcpy(buf[i], pos, sizeof pos); memcpy(buf[i] + sizeof pos, nrm, sizeof nrm);
    }
    glViewport(0, 0, W, H);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHT0);
    glEnable(GL_COLOR_MATERIAL);
    for (mode = 0; mode < 8; mode++) {
        double best = 1e9, draw = 0;
        int r, f;
        if (mode == 6) { memcpy(buf[0], inter, sizeof inter); memcpy(buf[1], inter, sizeof inter); }
        cpy = 0;
        for (r = 0; r < 6; r++) {
            double t0 = now(), t;
            for (f = 0; f < 4; f++) { double d = frame(mode); if (r) draw += d; }
            glFinish();
            t = (now() - t0) / 4;
            if (r && t < best) best = t;
        }
        printf("%-36s frame %6.3f ms   copy %6.3f ms   glDrawArrays %6.3f ms\n", name[mode], best * 1e3, cpy / 24, draw / 20);
    }
    return 0;
}
