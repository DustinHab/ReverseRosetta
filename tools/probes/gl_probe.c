/* gl_probe.c - which OpenGL renderers a program gets and whether an offscreen context draws:
 *   gcc -arch i386 -isysroot /Developer/SDKs/MacOSX10.5.sdk -o gl_probe_i386 gl_probe.c -framework OpenGL
 *   gcc -o gl_probe gl_probe.c -framework OpenGL
 *   gl_probe */
#include <OpenGL/OpenGL.h>
#include <OpenGL/CGLRenderers.h>
#include <OpenGL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

static double now(void)
{
    struct timeval t;
    gettimeofday(&t, 0);
    return t.tv_sec + t.tv_usec / 1e6;
}

static void try_format(const char *name, const CGLPixelFormatAttribute *a)
{
    CGLPixelFormatObj pf = 0;
    CGLContextObj ctx = 0;
    GLint n = 0, align = -1;
    CGLError err = CGLChoosePixelFormat(a, &pf, &n);
    enum { W = 256, H = 256 };
    unsigned *pix;
    double t0;
    int i;
    printf("%s: choose %d (%s), %d formats\n", name, err, CGLErrorString(err), n);
    if (err || !pf) return;
    err = CGLCreateContext(pf, 0, &ctx);
    printf("  context %d (%s)\n", err, CGLErrorString(err));
    CGLDestroyPixelFormat(pf);
    if (err || !ctx) return;
    pix = calloc(W * H, 4);
    err = CGLSetOffScreen(ctx, W, H, W * 4, pix);
    printf("  offscreen %d\n", err);
    CGLSetCurrentContext(ctx);
    printf("  renderer %s, version %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    printf("  unpack alignment %d\n", align);
    t0 = now();
    for (i = 0; i < 50; i++) {
        glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_TRIANGLES);
        glColor3f(1, 0, 0); glVertex2f(-0.9f, -0.9f);
        glColor3f(0, 1, 0); glVertex2f(0.9f, -0.9f);
        glColor3f(0, 0, 1); glVertex2f(0.0f, 0.9f);
        glEnd();
        glFinish();
    }
    printf("  50 frames %dx%d: %.3f s, pixel middle %08x corner %08x, error %x\n", W, H, now() - t0,
           pix[H / 2 * W + W / 2], pix[0], glGetError());
    CGLSetCurrentContext(0);
    CGLDestroyContext(ctx);
    free(pix);
}

int main(void)
{
    CGLRendererInfoObj ri;
    GLint n = 0, i, v;
    if (CGLQueryRendererInfo(0xffffffff, &ri, &n) == 0) {
        printf("%d renderers\n", n);
        for (i = 0; i < n; i++) {
            CGLDescribeRenderer(ri, i, kCGLRPRendererID, &v);
            printf("  renderer %08x", v);
            CGLDescribeRenderer(ri, i, kCGLRPAccelerated, &v);
            printf(" accelerated %d", v);
            CGLDescribeRenderer(ri, i, kCGLRPOffScreen, &v);
            printf(" offscreen %d\n", v);
        }
        CGLDestroyRendererInfo(ri);
    } else printf("no renderer info\n");
    {
        CGLPixelFormatAttribute a[] = { kCGLPFAOffScreen, kCGLPFAColorSize, 32, 0 };
        try_format("offscreen", a);
    }
    {
        CGLPixelFormatAttribute a[] = { kCGLPFARendererID, (CGLPixelFormatAttribute)kCGLRendererGenericFloatID,
                                        kCGLPFAOffScreen, kCGLPFAColorSize, 32, 0 };
        try_format("software (float) offscreen", a);
    }
    {
        CGLPixelFormatAttribute a[] = { kCGLPFAAccelerated, kCGLPFADoubleBuffer, kCGLPFAColorSize, 24, 0 };
        try_format("accelerated, double buffer", a);
    }
    return 0;
}
