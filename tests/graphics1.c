/* graphics1.c [SECONDS] - first probe for graphical programs, without AppKit: connection to the WindowServer,
 * display, a window via the CGS interface (as AppKit uses it), four color stripes with CoreGraphics, print the
 * window ID and keep the window open (tools/screen_grab then reads its content). */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ApplicationServices/ApplicationServices.h>

typedef int CGSConnectionID;
typedef int CGSWindowID;
typedef void *CGSRegionRef;
extern CGSConnectionID _CGSDefaultConnection(void);
extern CGError CGSNewRegionWithRect(const CGRect *r, CGSRegionRef *region);
extern CGError CGSReleaseRegion(CGSRegionRef region);
extern CGError CGSNewWindow(CGSConnectionID cid, int backing, float x, float y, CGSRegionRef shape, CGSWindowID *wid);
extern CGError CGSOrderWindow(CGSConnectionID cid, CGSWindowID wid, int loc, CGSWindowID ref);
extern CGError CGSSetWindowLevel(CGSConnectionID cid, CGSWindowID wid, int level);
extern CGError CGSSetWindowSharingState(CGSConnectionID cid, CGSWindowID wid, int share);
extern CGContextRef CGWindowContextCreate(CGSConnectionID cid, CGSWindowID wid, CFDictionaryRef options);

int main(int argc, char **argv)
{
    int sec = argc > 1 ? atoi(argv[1]) : 5, i;
    CGDirectDisplayID d = CGMainDisplayID();
    CGSConnectionID cid;
    CGSWindowID wid = 0;
    CGSRegionRef reg = 0;
    CGRect r = CGRectMake(0, 0, 240, 160);
    CGContextRef k;
    CGError f;
    static const float color[4][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 1, 1, 0 } };
    printf("display %u: %lu x %lu\n", (unsigned)d, (unsigned long)CGDisplayPixelsWide(d), (unsigned long)CGDisplayPixelsHigh(d));
    fflush(stdout);
    cid = _CGSDefaultConnection();
    printf("connection %d\n", cid);
    fflush(stdout);
    f = CGSNewRegionWithRect(&r, &reg);
    printf("region %d\n", f);
    f = CGSNewWindow(cid, 2, 120.0f, 120.0f, reg, &wid);
    printf("window %d (error %d)\n", wid, f);
    fflush(stdout);
    if (f || !wid) return 1;
    CGSSetWindowLevel(cid, wid, 1000);
    CGSSetWindowSharingState(cid, wid, 1);     /* readable by other processes (like AppKit) */
    k = CGWindowContextCreate(cid, wid, 0);
    printf("context %s\n", k ? "da" : "missing");
    fflush(stdout);
    if (!k) return 1;
    for (i = 0; i < 4; i++) {                   /* four horizontal stripes: red, green, blue, yellow (from the bottom) */
        CGContextSetRGBFillColor(k, color[i][0], color[i][1], color[i][2], 1.0f);
        CGContextFillRect(k, CGRectMake(0, 40 * i, 240, 40));
    }
    CGContextFlush(k);
    f = CGSOrderWindow(cid, wid, 1, 0);
    printf("front %d\n", f);
    printf("WINDOW %d\n", wid);
    fflush(stdout);
    sleep((unsigned)sec);
    CGContextRelease(k);
    CGSReleaseRegion(reg);
    return 0;
}
