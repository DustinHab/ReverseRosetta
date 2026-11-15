/* gl_attach.m MODE - can OpenGL draw into a window that another WindowServer connection of the same process owns?
 * (rosetta: the translated program's CoreGraphics owns the window, the native OpenGL has a connection of its own)
 *   own      the window's connection adds the surface and attaches the context (as AppKit does)
 *   cross    a second connection adds a surface to the window and attaches the context
 *   mixed    the window's connection adds the surface, the context is attached through the second connection
 *   overlay  the second connection makes a window of its own above the first one and draws there
 * Build: gcc-4.2 -o gl_attach gl_attach.m -framework Cocoa -framework OpenGL; run in the login session. */
#import <Cocoa/Cocoa.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef int CGSConnectionID, CGSWindowID, CGSSurfaceID;
typedef void *CGSRegionRef;
extern CGSConnectionID _CGSDefaultConnection(void);
extern CGError CGSNewConnection(void *attr, CGSConnectionID *cid);
extern CGError CGSAddSurface(CGSConnectionID cid, CGSWindowID wid, CGSSurfaceID *sid);
extern CGError CGSSetSurfaceBounds(CGSConnectionID cid, CGSWindowID wid, CGSSurfaceID sid, CGRect bounds);
extern CGError CGSOrderSurface(CGSConnectionID cid, CGSWindowID wid, CGSSurfaceID sid, int place, CGSSurfaceID rel);
extern CGError CGSNewRegionWithRect(const CGRect *r, CGSRegionRef *region);
extern CGError CGSNewWindow(CGSConnectionID cid, int backing, float left, float top, CGSRegionRef region, CGSWindowID *wid);
extern CGError CGSOrderWindow(CGSConnectionID cid, CGSWindowID wid, int place, CGSWindowID rel);
extern CGError CGSGetWindowBounds(CGSConnectionID cid, CGSWindowID wid, CGRect *r);
extern CGError CGSAddWindowToWindowMovementGroup(CGSConnectionID cid, CGSWindowID parent, CGSWindowID child);
extern CGLError CGLSetSurface(CGLContextObj ctx, CGSConnectionID cid, CGSWindowID wid, CGSSurfaceID sid);

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "own";
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSWindow *w;
    CGSConnectionID a, b = 0, gc;
    CGSWindowID wid, gw;
    CGSSurfaceID sid = 0;
    CGRect wb;
    CGLPixelFormatAttribute at[] = { kCGLPFAAccelerated, kCGLPFADoubleBuffer, kCGLPFAColorSize, 24, 0 };
    CGLPixelFormatObj pf;
    CGLContextObj ctx;
    GLint n;
    CGError e;
    int i;
    [NSApplication sharedApplication];
    w = [[NSWindow alloc] initWithContentRect:NSMakeRect(200, 200, 320, 240) styleMask:NSTitledWindowMask
                                       backing:NSBackingStoreBuffered defer:NO];
    [w setTitle:[NSString stringWithFormat:@"gl_attach %s", mode]];
    [w makeKeyAndOrderFront:nil];
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.3]];
    a = _CGSDefaultConnection();
    wid = (CGSWindowID)[w windowNumber];
    CGSGetWindowBounds(a, wid, &wb);
    e = CGSNewConnection(0, &b);
    printf("window %d of connection %d at %.0f %.0f %.0f %.0f; second connection %d (error %d)\n", wid, a,
           wb.origin.x, wb.origin.y, wb.size.width, wb.size.height, b, e);
    gc = a; gw = wid;
    if (!strcmp(mode, "cross")) gc = b;
    if (!strcmp(mode, "overlay")) {
        CGSRegionRef r;
        CGRect rr = CGRectMake(0, 0, 320, 240);
        CGSNewRegionWithRect(&rr, &r);
        e = CGSNewWindow(b, 2, wb.origin.x, wb.origin.y + wb.size.height - 240, r, &gw);
        printf("overlay window %d (error %d)\n", gw, e);
        printf("order above: %d\n", CGSOrderWindow(b, gw, 1, wid));
        printf("movement group (window's connection): %d\n", CGSAddWindowToWindowMovementGroup(a, wid, gw));
        gc = b;
    }
    e = CGSAddSurface(gc, gw, &sid);
    printf("add surface through %d: error %d, surface %d\n", gc, e, sid);
    if (e) return 1;
    printf("bounds %d\n", CGSSetSurfaceBounds(gc, gw, sid, CGRectMake(20, 20, 200, 150)));
    printf("order surface %d\n", CGSOrderSurface(gc, gw, sid, 1, 0));
    CGLChoosePixelFormat(at, &pf, &n);
    CGLCreateContext(pf, 0, &ctx);
    if (!strcmp(mode, "mixed")) gc = b;
    printf("attach through %d: %d\n", gc, CGLSetSurface(ctx, gc, gw, sid));
    CGLSetCurrentContext(ctx);
    printf("renderer %s\n", glGetString(GL_RENDERER));
    for (i = 0; i < 120; i++) {
        glViewport(0, 0, 200, 150);
        glClearColor(0.1f, 0.3f + 0.005f * (i % 100), 0.6f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_TRIANGLES);
        glColor3f(1, 0, 0); glVertex2f(-0.8f, -0.8f);
        glColor3f(0, 1, 0); glVertex2f(0.8f, -0.8f);
        glColor3f(1, 1, 0); glVertex2f(0.0f, 0.8f);
        glEnd();
        CGLFlushDrawable(ctx);
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
    }
    printf("gl error %x\n", glGetError());
    sleep(atoi(argc > 2 ? argv[2] : "3"));     /* time for a screen capture */
    [pool release];
    return 0;
}
