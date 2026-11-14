/* fonts.c - font metrics via CoreText/ATS (stage 5): glyphs, advances, bounding boxes, paths. Needs the ATSServer
   of the login session (tests/in_session.sh); the Intel version under rosetta must print the same as the PowerPC
   version (rosetta converts the ATS messages). */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>

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

int main(void)
{
    font(CFSTR("LucidaGrande"), 13.0);
    font(CFSTR("Helvetica-Bold"), 24.0);
    font(CFSTR("Times-Roman"), 9.5);
    return 0;
}
