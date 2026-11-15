/* gl_buffer_refill.c: native, how long glBufferSubData of 216000 bytes takes in the refilled-VBO workload, per source
   Build: gcc-4.2 -O2 -o /tmp/gl_buffer_refill tools/probes/gl_buffer_refill.c -framework OpenGL; run it in the login session (tests/in_session.sh). */
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <mach/mach_time.h>

#define W 256
#define H 256
#define NV 18000
static float pos[NV * 3], nrm[NV * 3];
static GLuint vbo;
static double ms(uint64_t d) { mach_timebase_info_data_t tb; mach_timebase_info(&tb); return (double)d * tb.numer / tb.denom / 1e6; }

static void frame(const void *src, int mode, double *tsub, double *tall)
{
    uint64_t t0, t1, t2;
    glClearColor(0.1f, 0.1f, 0.2f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    t0 = mach_absolute_time();
    if (mode == 0) glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof pos, src);
    else if (mode == 1) { void *m = glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY); memcpy(m, src, sizeof pos); glUnmapBuffer(GL_ARRAY_BUFFER); }
    else if (mode == 2) {                       /* APPLE_flush_buffer_range: no sync, flush the range */
        void *m;
        glBufferParameteriAPPLE(GL_ARRAY_BUFFER, GL_BUFFER_SERIALIZED_MODIFY_APPLE, GL_FALSE);
        glBufferParameteriAPPLE(GL_ARRAY_BUFFER, GL_BUFFER_FLUSHING_UNMAP_APPLE, GL_FALSE);
        m = glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
        memcpy(m, src, sizeof pos);
        glFlushMappedBufferRangeAPPLE(GL_ARRAY_BUFFER, 0, sizeof pos);
        glUnmapBuffer(GL_ARRAY_BUFFER);
        glBufferParameteriAPPLE(GL_ARRAY_BUFFER, GL_BUFFER_SERIALIZED_MODIFY_APPLE, GL_TRUE);
        glBufferParameteriAPPLE(GL_ARRAY_BUFFER, GL_BUFFER_FLUSHING_UNMAP_APPLE, GL_TRUE);
    }
    t1 = mach_absolute_time();
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, (const void *)0);
    glNormalPointer(GL_FLOAT, 0, (const void *)(sizeof pos));
    glDrawArrays(GL_TRIANGLES, 0, NV);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    t2 = mach_absolute_time();
    *tsub += ms(t1 - t0);
    *tall += ms(t2 - t0);
}

int main(void)
{
    CGLPixelFormatAttribute at[] = { kCGLPFAAccelerated, kCGLPFAPBuffer, kCGLPFAColorSize, 24, kCGLPFADepthSize, 16, 0 };
    CGLPixelFormatObj pf;
    CGLContextObj ctx;
    CGLPBufferObj pb;
    GLint n, vs = 0;
    int i, mode, src;
    float *heap = malloc(sizeof pos);
    CGLChoosePixelFormat(at, &pf, &n);
    CGLCreateContext(pf, 0, &ctx);
    CGLCreatePBuffer(W, H, GL_TEXTURE_2D, GL_RGBA, 0, &pb);
    CGLGetVirtualScreen(ctx, &vs);
    CGLSetPBuffer(ctx, pb, 0, 0, vs);
    CGLSetCurrentContext(ctx);
    for (i = 0; i < NV * 3; i++) { pos[i] = 0.5f * sinf(i * 0.01f); nrm[i] = cosf(i * 0.02f); }
    memcpy(heap, pos, sizeof pos);
    glViewport(0, 0, W, H);
    glEnable(GL_DEPTH_TEST);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof pos + sizeof nrm, 0, GL_STATIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof pos, pos);
    glBufferSubData(GL_ARRAY_BUFFER, sizeof pos, sizeof nrm, nrm);
    for (mode = 0; mode < 3; mode++)
        for (src = 0; src < 2; src++) {
            double ts = 0, ta = 0;
            int r;
            for (r = 0; r < 10; r++) { for (i = 0; i < 4; i++) frame(src ? (void *)heap : (void *)pos, mode, &ts, &ta); glFinish(); }
            printf("%s from %s: upload %.3f ms, frame %.3f ms\n", mode == 0 ? "glBufferSubData" : mode == 1 ? "map+memcpy" : "map unserialized",
                   src ? "malloc" : "static", ts / 40, ta / 40);
        }
    return 0;
}
