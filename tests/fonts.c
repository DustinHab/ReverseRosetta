/* fonts.c - font metrics via CoreText/ATS (stage 5): glyphs, advances, bounding boxes, paths, and small text drawn
   without antialiasing (bitmap strikes: the font server sends metrics and a 1-bit image per glyph). Needs the
   ATSServer of the login session (tests/in_session.sh); the Intel version under rosetta must print the same as the
   PowerPC version (rosetta converts the ATS messages). */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <string.h>

static void tally(void *info, const CGPathElement *e)
{
    int *n = info;
    if (e->type <= kCGPathElementCloseSubpath) n[e->type]++;
}

static void font(CFStringRef name, CGFloat size)
{
    static const UniChar z[] = { 'R', 'e', 'c', 'h', 'n', 'e', 'r', ' ', '0', '1', '2', '7', '9', '+', '=', 0xd7, 0xf7,
                                 0x2212, 'g', 'Q', 0xe4, 0xdf };
    enum { N = sizeof z / sizeof z[0] };
    CTFontRef f = CTFontCreateWithName(name, size, NULL);
    CFStringRef full = CTFontCopyFullName(f);
    char t[256];
    CGGlyph g[N];
    CGSize v[N];
    CGRect r[N];
    int i;
    CFStringGetCString(full, t, sizeof t, kCFStringEncodingUTF8);
    printf("%s %.1f: ascent %.3f descent %.3f leading %.3f units %u glyphs %ld\n", t, (double)size,
           (double)CTFontGetAscent(f), (double)CTFontGetDescent(f), (double)CTFontGetLeading(f),
           (unsigned)CTFontGetUnitsPerEm(f), (long)CTFontGetGlyphCount(f));
    CTFontGetGlyphsForCharacters(f, z, g, N);
    CTFontGetAdvancesForGlyphs(f, kCTFontDefaultOrientation, g, v, N);
    CTFontGetBoundingRectsForGlyphs(f, kCTFontDefaultOrientation, g, r, N);
    for (i = 0; i < N; i++) {
        int n[5] = { 0, 0, 0, 0, 0 };
        CGPathRef p = CTFontCreatePathForGlyph(f, g[i], NULL);
        CGRect b = p ? CGPathGetBoundingBox(p) : CGRectZero;
        if (p) CGPathApply(p, n, tally);
        printf("  %04x glyph %4u advance %7.3f bounds %7.3f %7.3f %7.3f %7.3f path %d/%d/%d/%d/%d %7.3f %7.3f\n",
               z[i], g[i], (double)v[i].width, (double)r[i].origin.x, (double)r[i].origin.y, (double)r[i].size.width,
               (double)r[i].size.height, n[0], n[1], n[2], n[3], n[4], (double)b.size.width, (double)b.size.height);
        if (p) CGPathRelease(p);
    }
    CFRelease(full);
    CFRelease(f);
}

/* text in a gray bitmap, printed row by row: # dark, + gray, . white */
static void bitmap(const char *name, CGFloat size, const char *text)
{
    enum { W = 100, H = 16 };
    static unsigned char px[W * H];
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceGray();
    CGContextRef c = CGBitmapContextCreate(px, W, H, 8, W, cs, kCGImageAlphaNone);
    int x, y;
    memset(px, 255, sizeof px);
    CGContextSetGrayFillColor(c, 0, 1);
    CGContextSetShouldAntialias(c, false);
    CGContextSelectFont(c, name, size, kCGEncodingMacRoman);
    CGContextShowTextAtPoint(c, 1, 4, text, strlen(text));
    CGContextFlush(c);
    printf("%s %.0f without antialiasing:\n", name, (double)size);
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) putchar(px[y * W + x] < 96 ? '#' : px[y * W + x] < 224 ? '+' : '.');
        putchar('\n');
    }
    CGContextRelease(c);
    CGColorSpaceRelease(cs);
}

int main(void)
{
    font(CFSTR("LucidaGrande"), 13.0);
    font(CFSTR("Helvetica-Bold"), 24.0);
    font(CFSTR("Times-Roman"), 9.5);
    bitmap("Monaco", 10.0, "Info Log 0127 gQ");
    bitmap("Monaco", 9.0, "Info Log 0127 gQ");
    bitmap("Geneva", 9.0, "Info Log 0127 gQ");
    bitmap("Geneva", 12.0, "mWM%@ wide");
    return 0;
}
