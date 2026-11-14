/* graphics2.m [SECONDS] - Cocoa probe: NSApplication, borderless NSWindow on the top level at (120,120) top
 * left, 240 x 160, four color stripes (from the top yellow, blue, green, red), event loop for SECONDS.
 * tools/screen_grab 120 120 240 160 reads the result from the framebuffer. */
#import <Cocoa/Cocoa.h>
#include <stdio.h>
#include <stdlib.h>

@interface Stripe : NSView
@end

/* ends the event loop: stop: takes effect only with the next event, hence an empty one afterwards */
@interface Finisher : NSObject
- (void)end:(id)x;
@end

@implementation Finisher
- (void)end:(id)x
{
    [NSApp stop:x];
    [NSApp postEvent:[NSEvent otherEventWithType:NSApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0
                                     windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:YES];
}
@end

@implementation Stripe
- (void)drawRect:(NSRect)r
{
    NSColor *colors[4];
    int i;
    colors[0] = [NSColor colorWithDeviceRed:1 green:0 blue:0 alpha:1];
    colors[1] = [NSColor colorWithDeviceRed:0 green:1 blue:0 alpha:1];
    colors[2] = [NSColor colorWithDeviceRed:0 green:0 blue:1 alpha:1];
    colors[3] = [NSColor colorWithDeviceRed:1 green:1 blue:0 alpha:1];
    for (i = 0; i < 4; i++) {
        [colors[i] set];
        NSRectFill(NSMakeRect(0, 40 * i, 240, 40));
    }
    printf("drawn\n");
    fflush(stdout);
    (void)r;
}
@end

int main(int argc, char **argv)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    double sec = argc > 1 ? atof(argv[1]) : 5;
    NSApplication *app = [NSApplication sharedApplication];
    NSRect screen = [[NSScreen mainScreen] frame];
    NSWindow *w;
    printf("screen %.0f x %.0f\n", screen.size.width, screen.size.height);
    fflush(stdout);
    w = [[NSWindow alloc] initWithContentRect:NSMakeRect(120, screen.size.height - 120 - 160, 240, 160)
                                    styleMask:NSBorderlessWindowMask backing:NSBackingStoreBuffered defer:NO];
    [w setLevel:NSScreenSaverWindowLevel];
    [w setContentView:[[[Stripe alloc] initWithFrame:NSMakeRect(0, 0, 240, 160)] autorelease]];
    [w orderFrontRegardless];
    [w display];
    printf("WINDOW %ld\n", (long)[w windowNumber]);
    fflush(stdout);
    [[[Finisher alloc] init] performSelector:@selector(end:) withObject:nil afterDelay:sec];
    [app run];
    printf("end of event loop\n");
    [pool release];
    return 0;
}
