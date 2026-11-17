/* mltefile.m - replacement for MLTEFile.bundle (i386): the plug-in through which MLTE (HIToolbox) reads and writes RTF
 * and RTFD with Cocoa's text system. The bundle on a PowerPC Mac has no i386 part and the 10.5.8 Combo Update does not
 * contain it, so TXNSetDataFromCFURLRef with an RTF file failed (-4960) in every Intel program: The Sims 2 Body Shop
 * ended right at startup because its licence text (RTF) could not be shown.
 *
 * Interface (from the PowerPC bundle; HIToolbox finds the functions by name):
 *   MLTEFile_ReadRTFDoc(url, &text, &runs, &documentAttributes)    RTF/RTFD file
 *   MLTEFile_ReadRTFData(data, &text, &runs, &documentAttributes)  RTF in memory
 *   MLTEFile_ReadRTFDData(data, &text, &runs, &documentAttributes) RTFD in memory
 *   MLTEFile_WriteRTFDoc(url, text, runs, documentAttributes)       to a file (RTFD if there are attachments)
 *   MLTEFile_FlattenRTFDoc(text, runs, documentAttributes, &data)   to memory
 * Results are returned retained (the caller releases them); 0 or -1 (reading), ioErr (writing). runs is an array of
 * dictionaries, one per attribute run: Location, Length (unsigned int), FontName, PointSize (float), EncodingScheme,
 * FontTraits (unsigned int), Foreground/BackgroundColorRed/Green/Blue (float), Underline, Alignment (int), TabStops,
 * Kern, Ligature, and for attachments AttachmentFileName, AttachmentFilePath, AttachmentData, AttachmentFileType.
 * Build: tools/intel_replacements.sh. */
#import <AppKit/AppKit.h>

/* color of the attribute key as three float numbers; NO if the run has none (a color in another color space is
   converted; Apple's plug-in took white then) */
static BOOL color_parts(NSDictionary *attrs, NSString *key, NSNumber **r, NSNumber **g, NSNumber **b)
{
    NSColor *c = [attrs objectForKey:key], *rgb;
    float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 1.0f;
    if (!c) return NO;
    NS_DURING
        rgb = [c colorUsingColorSpaceName:NSCalibratedRGBColorSpace];
        if (rgb) [rgb getRed:&cr green:&cg blue:&cb alpha:&ca];
    NS_HANDLER
        cr = cg = cb = 1.0f;
    NS_ENDHANDLER
    *r = [NSNumber numberWithFloat:cr];
    *g = [NSNumber numberWithFloat:cg];
    *b = [NSNumber numberWithFloat:cb];
    return YES;
}

static void put(NSMutableDictionary *d, id value, NSString *key)
{
    if (value) [d setObject:value forKey:key];
}

/* the attribute runs of s (retained array); rtf: attachments by file name only (RTFD: contents too) */
static NSMutableArray *runs_of(NSAttributedString *s, BOOL rtf)
{
    NSMutableArray *a = [[NSMutableArray alloc] init];
    unsigned len = [s length], i = 0;
    while (i < len) {
        NSRange r;
        NSDictionary *attrs = [s attributesAtIndex:i effectiveRange:&r];
        NSMutableDictionary *d = [[NSMutableDictionary alloc] init];
        NSTextAttachment *att = [attrs objectForKey:NSAttachmentAttributeName];
        NSFont *font = [attrs objectForKey:NSFontAttributeName];
        NSParagraphStyle *ps = [attrs objectForKey:NSParagraphStyleAttributeName];
        NSNumber *cr, *cg, *cb;
        [d setObject:[NSNumber numberWithUnsignedInt:r.location] forKey:@"Location"];
        [d setObject:[NSNumber numberWithUnsignedInt:r.length] forKey:@"Length"];
        if (att) {
            NSFileWrapper *fw = [att fileWrapper];
            if (rtf) put(d, [fw filename], @"AttachmentFileName");
            else if ([fw isRegularFile]) {
                put(d, [fw filename], @"AttachmentFileName");
                if ([fw filename] && [fw regularFileContents])        /* (keyed by the file name, as Apple's) */
                    [d setObject:[fw regularFileContents] forKey:[fw filename]];
            } else if ([fw isSymbolicLink]) put(d, [fw symbolicLinkDestination], @"AttachmentFilePath");
        }
        if (font) {
            put(d, [font fontName], @"FontName");
            [d setObject:[NSNumber numberWithFloat:[font pointSize]] forKey:@"PointSize"];
            put(d, [font encodingScheme], @"EncodingScheme");
            [d setObject:[NSNumber numberWithUnsignedInt:[[NSFontManager sharedFontManager] traitsOfFont:font]]
                  forKey:@"FontTraits"];
        }
        if (color_parts(attrs, NSForegroundColorAttributeName, &cr, &cg, &cb)) {
            [d setObject:cr forKey:@"ForegroundColorRed"];
            [d setObject:cg forKey:@"ForegroundColorGreen"];
            [d setObject:cb forKey:@"ForegroundColorBlue"];
        }
        if (color_parts(attrs, NSBackgroundColorAttributeName, &cr, &cg, &cb)) {
            [d setObject:cr forKey:@"BackgroundColorRed"];
            [d setObject:cg forKey:@"BackgroundColorGreen"];
            [d setObject:cb forKey:@"BackgroundColorBlue"];
        }
        put(d, [attrs objectForKey:NSUnderlineStyleAttributeName], @"Underline");
        if (ps) {
            [d setObject:[NSNumber numberWithInt:[ps alignment]] forKey:@"Alignment"];
            put(d, [ps tabStops], @"TabStops");
        }
        put(d, [attrs objectForKey:NSKernAttributeName], @"Kern");
        put(d, [attrs objectForKey:NSLigatureAttributeName], @"Ligature");
        [a addObject:d];
        [d release];
        if (!r.length) break;
        i = r.location + r.length;
    }
    return a;
}

/* text, runs and document attributes of a string read by Cocoa (s is released) */
static OSStatus give(NSAttributedString *s, BOOL rtf, CFStringRef *text, CFArrayRef *runs, CFDictionaryRef *doc)
{
    if (!s) return -1;
    if (doc && *doc) [(id)*doc retain];
    if (text) *text = (CFStringRef)[[s string] retain];
    if (runs) *runs = (CFArrayRef)runs_of(s, rtf);
    [s release];
    return 0;
}

OSStatus MLTEFile_ReadRTFDoc(CFURLRef url, CFStringRef *text, CFArrayRef *runs, CFDictionaryRef *doc)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    OSStatus err = -1;
    NS_DURING
        err = give([[NSAttributedString alloc] initWithURL:(NSURL *)url documentAttributes:(NSDictionary **)doc], YES,
                   text, runs, doc);
    NS_HANDLER
        err = -1;
    NS_ENDHANDLER
    [pool release];
    return err;
}

OSStatus MLTEFile_ReadRTFData(CFDataRef data, CFStringRef *text, CFArrayRef *runs, CFDictionaryRef *doc)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    OSStatus err = -1;
    NS_DURING
        err = give([[NSAttributedString alloc] initWithRTF:(NSData *)data documentAttributes:(NSDictionary **)doc], YES,
                   text, runs, doc);
    NS_HANDLER
        err = -1;
    NS_ENDHANDLER
    [pool release];
    return err;
}

OSStatus MLTEFile_ReadRTFDData(CFDataRef data, CFStringRef *text, CFArrayRef *runs, CFDictionaryRef *doc)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    OSStatus err = -1;
    NS_DURING
        err = give([[NSAttributedString alloc] initWithRTFD:(NSData *)data documentAttributes:(NSDictionary **)doc], NO,
                   text, runs, doc);
    NS_HANDLER
        err = -1;
    NS_ENDHANDLER
    [pool release];
    return err;
}

/* text with the runs as an attributed string (retained); *attachments: some run has a file attachment */
static NSMutableAttributedString *build(NSString *text, NSArray *runs, BOOL *attachments)
{
    NSMutableAttributedString *s = [[NSMutableAttributedString alloc] initWithString:text ? text : @""];
    unsigned n = [runs count], i, len = [s length];
    *attachments = NO;
    for (i = 0; i < n; i++) {
        NSDictionary *run = [runs objectAtIndex:i];
        NSMutableDictionary *attrs = [[NSMutableDictionary alloc] init];
        unsigned loc = [[run objectForKey:@"Location"] unsignedIntValue], l = [[run objectForKey:@"Length"] unsignedIntValue];
        NSString *name = [run objectForKey:@"AttachmentFileName"], *fontName = [run objectForKey:@"FontName"];
        NSNumber *r = [run objectForKey:@"ForegroundColorRed"], *g = [run objectForKey:@"ForegroundColorGreen"];
        NSNumber *b = [run objectForKey:@"ForegroundColorBlue"], *align = [run objectForKey:@"Alignment"];
        BOOL here = NO;
        id v;
        if (name) {
            NSFileWrapper *fw = [[NSFileWrapper alloc] initRegularFileWithContents:[run objectForKey:@"AttachmentData"]];
            NSMutableDictionary *fa = [[NSMutableDictionary alloc] init];
            NSTextAttachment *att;
            [fw setFilename:name];
            [fw setPreferredFilename:name];
            put(fa, [run objectForKey:@"AttachmentFileType"], NSFileHFSTypeCode);
            [fa setObject:NSFileTypeRegular forKey:NSFileType];
            [fa setObject:[NSNumber numberWithUnsignedShort:0644] forKey:NSFilePosixPermissions];
            [fw setFileAttributes:fa];
            [fa release];
            att = [[NSTextAttachment alloc] initWithFileWrapper:fw];
            [fw release];
            [attrs setObject:att forKey:NSAttachmentAttributeName];
            [att release];
            *attachments = here = YES;
        }
        if (fontName) {
            NSFont *f = [NSFont fontWithName:fontName size:[[run objectForKey:@"PointSize"] floatValue]];
            unsigned traits = [[run objectForKey:@"FontTraits"] unsignedIntValue];
            if (f && traits) f = [[NSFontManager sharedFontManager] convertFont:f toHaveTrait:traits];
            put(attrs, f, NSFontAttributeName);
        }
        if (r && g && b)
            [attrs setObject:[NSColor colorWithCalibratedRed:[r floatValue] green:[g floatValue] blue:[b floatValue] alpha:1]
                      forKey:NSForegroundColorAttributeName];
        r = [run objectForKey:@"BackgroundColorRed"]; g = [run objectForKey:@"BackgroundColorGreen"];
        b = [run objectForKey:@"BackgroundColorBlue"];
        if (r && g && b)
            [attrs setObject:[NSColor colorWithCalibratedRed:[r floatValue] green:[g floatValue] blue:[b floatValue] alpha:1]
                      forKey:NSBackgroundColorAttributeName];
        if ((v = [run objectForKey:@"Underline"])) [attrs setObject:v forKey:NSUnderlineStyleAttributeName];
        if (align) {
            NSMutableParagraphStyle *ps = [[NSMutableParagraphStyle alloc] init];
            [ps setAlignment:[align intValue]];
            [attrs setObject:ps forKey:NSParagraphStyleAttributeName];
            [ps release];
        }
        if ((v = [run objectForKey:@"Kern"])) [attrs setObject:v forKey:NSKernAttributeName];
        if ((v = [run objectForKey:@"Ligature"])) [attrs setObject:v forKey:NSLigatureAttributeName];
        if (loc <= len && l <= len - loc) {
            NS_DURING
                [s setAttributes:attrs range:NSMakeRange(loc, l)];
                if (here) [s fixAttachmentAttributeInRange:NSMakeRange(loc, l)];
            NS_HANDLER
            NS_ENDHANDLER
        }
        [attrs release];
    }
    return s;
}

/* url: write the file; otherwise *out = the RTF/RTFD data (retained) */
static OSStatus write_doc(CFURLRef url, CFStringRef text, CFArrayRef runs, CFDictionaryRef doc, CFDataRef *out)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    BOOL attachments;
    NSMutableAttributedString *s = build((NSString *)text, (NSArray *)runs, &attachments);
    NSRange all = NSMakeRange(0, [s length]);
    NSFileWrapper *fw = nil;
    NSData *data = nil;
    OSStatus err = 0;
    NS_DURING
        if (attachments) {
            if (out) data = [s RTFDFromRange:all documentAttributes:(NSDictionary *)doc];
            else fw = [s RTFDFileWrapperFromRange:all documentAttributes:(NSDictionary *)doc];
        } else {
            data = [s RTFFromRange:all documentAttributes:(NSDictionary *)doc];
            if (!out && data) fw = [[[NSFileWrapper alloc] initRegularFileWithContents:data] autorelease];
        }
    NS_HANDLER
        data = nil; fw = nil;
    NS_ENDHANDLER
    if (out) {
        *out = (CFDataRef)[data retain];
        if (!data) err = -36;
    } else {
        NSString *path = [[(NSURL *)url path] retain];
        BOOL ok = path && fw && [fw writeToFile:path atomically:NO updateFilenames:YES];
        [path release];
        if (!ok) err = -36;                     /* ioErr */
    }
    [s release];
    [pool release];
    return err;
}

OSStatus MLTEFile_WriteRTFDoc(CFURLRef url, CFStringRef text, CFArrayRef runs, CFDictionaryRef doc)
{
    return write_doc(url, text, runs, doc, 0);
}

OSStatus MLTEFile_FlattenRTFDoc(CFStringRef text, CFArrayRef runs, CFDictionaryRef doc, CFDataRef *out)
{
    return out ? write_doc(0, text, runs, doc, out) : -50;
}
