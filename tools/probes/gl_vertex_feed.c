/* gl_vertex_feed.c: native, cost of glDrawArrays (CPU) and of a frame for ways to feed 18000 lit, colored vertices:
   client arrays, a streaming buffer object with or without the colors, interleaved
   Build: gcc-4.2 -O2 -o /tmp/gl_vertex_feed tools/probes/gl_vertex_feed.c -framework OpenGL; run it in the login session (tests/in_session.sh). */
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
static GLuint sb[3];
static int sk;
static double ms(uint64_t d) { mach_timebase_info_data_t tb; mach_timebase_info(&tb); return (double)d * tb.numer / tb.denom / 1e6; }
static double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec / 1e6; }

static void *stream(size_t n, int usage)
{
    void *m;
    sk = (sk + 1) % 3;
    glBindBuffer(GL_ARRAY_BUFFER, sb[sk]);
    glBufferData(GL_ARRAY_BUFFER, n, 0, usage);
    m = glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
    return m;
}

static double frame(int mode)
{
    uint64_t t0;
    double d;
    char *m;
    glClearColor(0.1f, 0.1f, 0.2f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    switch (mode) {
    case 0:                                     /* client arrays */
        glVertexPointer(3, GL_FLOAT, 0, pos); glNormalPointer(GL_FLOAT, 0, nrm); glColorPointer(4, GL_UNSIGNED_BYTE, 0, col);
        break;
    case 1:                                     /* positions and normals streamed, colors client */
        m = stream(sizeof pos + sizeof nrm, GL_STREAM_DRAW);
        memcpy(m, pos, sizeof pos); memcpy(m + sizeof pos, nrm, sizeof nrm);
        glUnmapBuffer(GL_ARRAY_BUFFER);
        glVertexPointer(3, GL_FLOAT, 0, 0); glNormalPointer(GL_FLOAT, 0, (void *)sizeof pos);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glColorPointer(4, GL_UNSIGNED_BYTE, 0, col);
        break;
    case 2: case 3:                             /* all three streamed (3: GL_DYNAMIC_DRAW) */
        m = stream(sizeof pos + sizeof nrm + sizeof col, mode == 2 ? GL_STREAM_DRAW : GL_DYNAMIC_DRAW);
        memcpy(m, pos, sizeof pos); memcpy(m + sizeof pos, nrm, sizeof nrm); memcpy(m + 2 * sizeof pos, col, sizeof col);
        glUnmapBuffer(GL_ARRAY_BUFFER);
        glVertexPointer(3, GL_FLOAT, 0, 0); glNormalPointer(GL_FLOAT, 0, (void *)sizeof pos);
        glColorPointer(4, GL_UNSIGNED_BYTE, 0, (void *)(2 * sizeof pos));
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        break;
    case 4:                                     /* interleaved client */
        glVertexPointer(3, GL_FLOAT, 28, inter[0].p); glNormalPointer(GL_FLOAT, 28, inter[0].n); glColorPointer(4, GL_UNSIGNED_BYTE, 28, inter[0].c);
        break;
    case 5:                                     /* interleaved streamed, all from the buffer */
        m = stream(sizeof inter, GL_STREAM_DRAW);
        memcpy(m, inter, sizeof inter);
        glUnmapBuffer(GL_ARRAY_BUFFER);
        glVertexPointer(3, GL_FLOAT, 28, 0); glNormalPointer(GL_FLOAT, 28, (void *)12); glColorPointer(4, GL_UNSIGNED_BYTE, 28, (void *)24);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        break;
    case 6:                                     /* interleaved streamed, colors from client memory */
        m = stream(sizeof inter, GL_STREAM_DRAW);
        memcpy(m, inter, sizeof inter);
        glUnmapBuffer(GL_ARRAY_BUFFER);
        glVertexPointer(3, GL_FLOAT, 28, 0); glNormalPointer(GL_FLOAT, 28, (void *)12);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glColorPointer(4, GL_UNSIGNED_BYTE, 28, inter[0].c);
        break;
    case 7:                                     /* streamed with glBufferData from memory, all three */
        sk = (sk + 1) % 3;
        glBindBuffer(GL_ARRAY_BUFFER, sb[sk]);
        glBufferData(GL_ARRAY_BUFFER, sizeof inter, inter, GL_STREAM_DRAW);
        glVertexPointer(3, GL_FLOAT, 28, 0); glNormalPointer(GL_FLOAT, 28, (void *)12); glColorPointer(4, GL_UNSIGNED_BYTE, 28, (void *)24);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        break;
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
    static const char *name[] = { "client arrays", "pos+nrm streamed, colors client", "all streamed", "all streamed DYNAMIC",
                                  "interleaved client", "interleaved streamed", "interleaved streamed, colors client",
                                  "interleaved glBufferData" };
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
    glViewport(0, 0, W, H);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHT0);
    glEnable(GL_COLOR_MATERIAL);
    glGenBuffers(3, sb);
    for (mode = 0; mode < 8; mode++) {
        double best = 1e9, draw = 0;
        int r, f;
        for (r = 0; r < 6; r++) {
            double t0 = now(), t;
            for (f = 0; f < 4; f++) draw += frame(mode);
            glFinish();
            t = (now() - t0) / 4;
            if (r && t < best) best = t;
        }
        printf("%-40s frame %6.3f ms   glDrawArrays %6.3f ms\n", name[mode], best * 1e3, draw / 24);
    }
    return 0;
}
