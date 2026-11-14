/* screen_grab.c X Y WIDTH HEIGHT [FILE.png] - read a rectangle of the screen (top left, points) directly from the
 * framebuffer: color in the middle of four horizontal stripes and of one corner, optionally as PNG. Meant for probe
 * windows on the top level (only their pixels). CGWindowListCreateImage returns nothing on this Leopard.
 * Build: gcc-4.2 -o screen_grab screen_grab.c -framework ApplicationServices -framework CoreServices -framework IOKit */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ApplicationServices/ApplicationServices.h>
#include <IOKit/IOKitLib.h>

/* power state of the display (4 = on, below that dimmed or sleeping) */
static int display_tier(void)
{
    io_service_t s = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("IODisplayWrangler"));
    CFDictionaryRef pm;
    int st = -1;
    if (!s) return -1;
    pm = IORegistryEntryCreateCFProperty(s, CFSTR("IOPowerManagement"), 0, 0);
    if (pm) {
        CFNumberRef n = CFDictionaryGetValue(pm, CFSTR("CurrentPowerState"));
        if (n) CFNumberGetValue(n, kCFNumberIntType, &st);
        CFRelease(pm);
    }
    IOObjectRelease(s);
    return st;
}

/* put the display back to sleep (as before) */
static void display_sleep(void)
{
    io_registry_entry_t s = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("IODisplayWrangler"));
    if (!s) return;
    IORegistryEntrySetCFProperty(s, CFSTR("IORequestIdle"), kCFBooleanTrue);
    IOObjectRelease(s);
}

int main(int argc, char **argv)
{
    CGDirectDisplayID d = CGMainDisplayID();
    int x, y, b, h, i, j, bpp, line, before, t;
    uint8_t *fb, *px;
    if (argc < 5) { fprintf(stderr, "screen_grab X Y WIDTH HEIGHT [FILE.png]\n"); return 2; }
    x = atoi(argv[1]); y = atoi(argv[2]); b = atoi(argv[3]); h = atoi(argv[4]);
    before = display_tier();
    if (before != 4) {                          /* sleeping display: the WindowServer does not composite an image */
        UpdateSystemActivity(UsrActivity);
        for (t = 0; t < 50 && display_tier() != 4; t++) usleep(100000);
        sleep(2);
    }
    printf("display power state before %d, now %d\n", before, display_tier());
    fb = CGDisplayBaseAddress(d);
    bpp = (int)CGDisplayBitsPerPixel(d);
    line = (int)CGDisplayBytesPerRow(d);
    printf("framebuffer %p, %d bits, %d bytes per row\n", (void *)fb, bpp, line);
    if (!fb || bpp != 32) return 1;
    px = malloc((size_t)b * (size_t)h * 4);
    for (j = 0; j < h; j++) memcpy(px + 4 * b * j, fb + (y + j) * line + 4 * x, (size_t)b * 4);   /* xRGB */
    for (i = 0; i < 5; i++) {
        int sx = i < 4 ? b / 2 : 2, sy = i < 4 ? h * (2 * i + 1) / 8 : 2;
        uint8_t *p = px + 4 * (sy * b + sx);
        printf("%s %3d%% from top: R %3u G %3u B %3u\n", i < 4 ? "stripe  " : "corner  ", 100 * sy / h, p[1], p[2], p[3]);
    }
    if (argc > 5) {
        CGContextRef k = CGBitmapContextCreate(px, (size_t)b, (size_t)h, 8, (size_t)b * 4, CGColorSpaceCreateDeviceRGB(),
                                               kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Big);
        CGImageRef image = CGBitmapContextCreateImage(k);
        CFURLRef u = CFURLCreateFromFileSystemRepresentation(0, (const UInt8 *)argv[5], (CFIndex)strlen(argv[5]), false);
        CGImageDestinationRef z = CGImageDestinationCreateWithURL(u, CFSTR("public.png"), 1, 0);
        CGImageDestinationAddImage(z, image, 0);
        CGImageDestinationFinalize(z);
        printf("saved: %s\n", argv[5]);
    }
    if (before != 4) display_sleep();
    return 0;
}
