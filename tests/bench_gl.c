/* bench_gl.c [ROUNDS [FILTER]] - cost of OpenGL calls: the same workloads built for i386 (translated) and for PowerPC
 * (native), drawn by the graphics chip into a 256x256 pbuffer. Per workload: milliseconds per frame (fastest of the
 * rounds), calls per second and a checksum of the rendered image, which must be the same translated and native.
 * Runs in the login session (tests/in_session.sh): a hardware context needs the WindowServer. */
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>

#define W 256
#define H 256
#define NV 18000                                /* vertices of the meshes (6000 triangles) */

static float pos[NV * 3], nrm[NV * 3], tc[NV * 2];
static unsigned char col[NV * 4];
static unsigned short idx[NV];
static unsigned int tex[W * H];
static GLuint texname[2], list, vbo;

static double now(void)
{
    struct timeval t;
    gettimeofday(&t, 0);
    return t.tv_sec + t.tv_usec / 1e6;
}

static unsigned checksum(void)
{
    static unsigned px[W * H];
    unsigned h = 2166136261u, i;
    glReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
    for (i = 0; i < W * H; i++) h = (h ^ px[i]) * 16777619u;
    return h;
}

static void frame_begin(void)
{
    glClearColor(0.1f, 0.1f, 0.2f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

/* ---------- workloads: one frame each, returns the number of GL calls ---------- */

static long w_immediate(void)
{
    int i;
    frame_begin();
    glBegin(GL_TRIANGLES);
    for (i = 0; i < NV; i++) {
        glColor3f(col[4 * i] / 255.0f, col[4 * i + 1] / 255.0f, col[4 * i + 2] / 255.0f);
        glNormal3f(nrm[3 * i], nrm[3 * i + 1], nrm[3 * i + 2]);
        glVertex3f(pos[3 * i], pos[3 * i + 1], pos[3 * i + 2]);
    }
    glEnd();
    return 3L * NV + 2;
}

static long w_immediate_v(void)
{
    int i;
    frame_begin();
    glBegin(GL_TRIANGLES);
    for (i = 0; i < NV; i++) {
        glColor4ubv(col + 4 * i);
        glNormal3fv(nrm + 3 * i);
        glVertex3fv(pos + 3 * i);
    }
    glEnd();
    return 3L * NV + 2;
}

static long w_textured(void)
{
    int i;
    frame_begin();
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texname[0]);
    glBegin(GL_TRIANGLES);
    for (i = 0; i < NV; i++) {
        glTexCoord2f(tc[2 * i], tc[2 * i + 1]);
        glVertex2f(pos[3 * i], pos[3 * i + 1]);
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);
    return 2L * NV + 5;
}

static long w_state(void)
{
    int i;
    frame_begin();
    for (i = 0; i < 2000; i++) {
        glPushMatrix();
        glTranslatef(pos[3 * i] * 0.5f, pos[3 * i + 1] * 0.5f, 0.0f);
        glRotatef((float)(i % 360), 0.0f, 0.0f, 1.0f);
        glScalef(0.05f, 0.05f, 1.0f);
        glBindTexture(GL_TEXTURE_2D, texname[i & 1]);
        if (i & 2) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        glColor4ub(col[4 * i], col[4 * i + 1], col[4 * i + 2], 200);
        glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
        glPopMatrix();
    }
    glDisable(GL_BLEND);
    return 8L * 2000;
}

static long w_lists(void)
{
    int i;
    frame_begin();
    for (i = 0; i < 40; i++) {
        glLoadIdentity();
        glRotatef((float)i * 9.0f, 0.0f, 0.0f, 1.0f);
        glScalef(0.9f, 0.9f, 1.0f);
        glCallList(list);
    }
    return 4L * 40;
}

static long w_arrays(void)
{
    frame_begin();
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, pos);
    glNormalPointer(GL_FLOAT, 0, nrm);
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, col);
    glDrawArrays(GL_TRIANGLES, 0, NV);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    return 10;
}

static long w_elements(void)
{
    frame_begin();
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, pos);
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, col);
    glDrawElements(GL_TRIANGLES, NV, GL_UNSIGNED_SHORT, idx);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    return 7;
}

static long w_vbo(void)
{
    frame_begin();
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, (const void *)0);
    glNormalPointer(GL_FLOAT, 0, (const void *)(sizeof pos));
    glDrawArrays(GL_TRIANGLES, 0, NV);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return 9;
}

/* interleaved vertices as games keep them: position, normal, color in one structure */
struct vtx { float p[3], n[3]; unsigned char c[4]; };
static struct vtx inter[NV];
static long w_interleaved(void)
{
    frame_begin();
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, sizeof inter[0], inter[0].p);
    glNormalPointer(GL_FLOAT, sizeof inter[0], inter[0].n);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof inter[0], inter[0].c);
    glDrawArrays(GL_TRIANGLES, 0, NV);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    return 10;
}

/* a buffer object filled again every frame (animated geometry) */
static long w_vbo_dynamic(void)
{
    frame_begin();
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof pos, pos);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, (const void *)0);
    glNormalPointer(GL_FLOAT, 0, (const void *)(sizeof pos));
    glDrawArrays(GL_TRIANGLES, 0, NV);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return 10;
}

static long w_upload(void)
{
    int i;
    frame_begin();
    for (i = 0; i < 4; i++) {
        tex[i * 97] ^= 0x00ff00ffu;
        glBindTexture(GL_TEXTURE_2D, texname[1]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, tex);
    }
    glEnable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(-1, -1); glTexCoord2f(1, 0); glVertex2f(1, -1);
    glTexCoord2f(1, 1); glVertex2f(1, 1); glTexCoord2f(0, 1); glVertex2f(-1, 1);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    return 8 + 12;
}

static long w_readback(void)
{
    static unsigned px[W * H];
    int i;
    w_textured();
    for (i = 0; i < 4; i++) glReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
    return 4;
}

static long w_get(void)
{
    GLint v[4];
    GLfloat m[16];
    int i;
    frame_begin();
    for (i = 0; i < 2000; i++) {
        glGetIntegerv(GL_VIEWPORT, v);
        glGetFloatv(GL_MODELVIEW_MATRIX, m);
        glGetIntegerv(GL_MATRIX_MODE, v);
    }
    return 3L * 2000;
}

static const struct { const char *name; long (*fn)(void); } work[] = {
    { "immediate glColor3f/glNormal3f/glVertex3f", w_immediate },
    { "immediate vectors (ubv, fv)", w_immediate_v },
    { "immediate textured glTexCoord2f/glVertex2f", w_textured },
    { "matrix and state changes", w_state },
    { "display lists", w_lists },
    { "vertex arrays glDrawArrays", w_arrays },
    { "glDrawElements (ushort indices)", w_elements },
    { "vertex buffer object", w_vbo },
    { "interleaved arrays (stride 28)", w_interleaved },
    { "buffer object refilled every frame", w_vbo_dynamic },
    { "texture upload 256x256 BGRA", w_upload },
    { "glReadPixels 256x256", w_readback },
    { "glGet", w_get },
};

static void scene(void)
{
    int i;
    unsigned s = 12345;
    for (i = 0; i < NV; i++) {
        float a = (float)i * 0.013f, r = 0.2f + 0.7f * (float)((i * 7919) % 1000) / 1000.0f;
        pos[3 * i] = r * cosf(a);
        pos[3 * i + 1] = r * sinf(a * 1.7f);
        pos[3 * i + 2] = -0.5f + (float)(i % 100) / 100.0f;
        nrm[3 * i] = cosf(a); nrm[3 * i + 1] = sinf(a); nrm[3 * i + 2] = 0.3f;
        tc[2 * i] = (pos[3 * i] + 1.0f) * 0.5f; tc[2 * i + 1] = (pos[3 * i + 1] + 1.0f) * 0.5f;
        s = s * 1103515245u + 12345u;
        col[4 * i] = (unsigned char)(s >> 24); col[4 * i + 1] = (unsigned char)(s >> 16);
        col[4 * i + 2] = (unsigned char)(s >> 8); col[4 * i + 3] = 255;
        idx[i] = (unsigned short)((i * 37) % NV);
        memcpy(inter[i].p, pos + 3 * i, 12);
        memcpy(inter[i].n, nrm + 3 * i, 12);
        memcpy(inter[i].c, col + 4 * i, 4);
    }
    for (i = 0; i < W * H; i++) tex[i] = 0xff000000u | (unsigned)((i * 2654435761u) >> 8);
}

int main(int argc, char **argv)
{
    CGLPixelFormatAttribute at[] = { kCGLPFAAccelerated, kCGLPFAPBuffer, kCGLPFAColorSize, 24, kCGLPFADepthSize, 16, 0 };
    CGLPixelFormatObj pf;
    CGLContextObj ctx;
    CGLPBufferObj pb;
    GLint n, vs = 0;
    int rounds = argc > 1 ? atoi(argv[1]) : 5, k, r;
    if (CGLChoosePixelFormat(at, &pf, &n) || !pf) { printf("no accelerated pbuffer format\n"); return 1; }
    if (CGLCreateContext(pf, 0, &ctx) || CGLCreatePBuffer(W, H, GL_TEXTURE_2D, GL_RGBA, 0, &pb)) { printf("no context\n"); return 1; }
    CGLGetVirtualScreen(ctx, &vs);
    if (CGLSetPBuffer(ctx, pb, 0, 0, vs)) { printf("no pbuffer\n"); return 1; }
    CGLSetCurrentContext(ctx);
    printf("renderer %s\n", glGetString(GL_RENDERER));
    scene();
    glViewport(0, 0, W, H);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHT0);
    glEnable(GL_COLOR_MATERIAL);
    glGenTextures(2, texname);
    for (k = 0; k < 2; k++) {
        glBindTexture(GL_TEXTURE_2D, texname[k]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, tex);
    }
    list = glGenLists(1);
    glNewList(list, GL_COMPILE);
    glBegin(GL_TRIANGLES);
    for (k = 0; k < 6000; k++) { glColor4ubv(col + 4 * k); glVertex3fv(pos + 3 * k); }
    glEnd();
    glEndList();
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof pos + sizeof nrm, 0, GL_STATIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof pos, pos);
    glBufferSubData(GL_ARRAY_BUFFER, sizeof pos, sizeof nrm, nrm);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    for (k = 0; k < (int)(sizeof work / sizeof work[0]); k++) {
        double best = 1e9;
        long calls = 0;
        unsigned sum;
        if (argc > 2 && !strstr(work[k].name, argv[2])) continue;         /* only the workloads named so */
        for (r = 0; r < rounds; r++) {
            double t0 = now(), t;
            int f;
            for (f = 0; f < 4; f++) calls = work[k].fn();
            glFinish();
            t = (now() - t0) / 4;
            if (t < best) best = t;
        }
        sum = checksum();
        printf("%-44s %8.3f ms/frame %8.2f Mcalls/s  image %08x\n", work[k].name, best * 1e3, calls / best / 1e6, sum);
    }
    printf("gl error %x\n", glGetError());
    return 0;
}
