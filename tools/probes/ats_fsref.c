/* ats_fsref.c - the file of a font through the font server (ATSFontGetFileReference, ATS message GetFSRefForFileToken)
 * and its path; natively and translated (in the login session: the font server of the session). Build:
 * gcc-4.2 -arch ppc -arch i386 -isysroot /Developer/SDKs/MacOSX10.5.sdk -framework ApplicationServices */
#include <stdio.h>
#include <ApplicationServices/ApplicationServices.h>

int main(void)
{
    const char *names[] = { "LucidaGrande", "Helvetica", "Courier" };
    int i;
    for (i = 0; i < 3; i++) {
        CFStringRef n = CFStringCreateWithCString(0, names[i], kCFStringEncodingUTF8);
        ATSFontRef f = ATSFontFindFromPostScriptName(n, kATSOptionFlagsDefault);
        FSRef r;
        UInt8 path[1024] = "";
        OSStatus e = f ? ATSFontGetFileReference(f, &r) : -1;
        OSStatus p = e == noErr ? FSRefMakePath(&r, path, sizeof path) : -1;
        printf("%s: font %s, file reference %d, path %d %s\n", names[i], f ? "found" : "missing", (int)e, (int)p, (const char *)path);
        CFRelease(n);
    }
    return 0;
}
