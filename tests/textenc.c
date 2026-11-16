/* textenc.c - text encodings: CFString conversions and the Text Encoding Converter (TextToUnicodeInfo and the TEC*
 * converter API) for MacRoman, Shift-JIS, EUC-CN and EUC-KR, and between Unicode forms. The TEC converters need the
 * plugins in /System/Library/TextEncodings, which have no i386 part: under rosetta they come from the system's CarbonCore
 * (tec.c). UTF-16 is printed as code units; the output must match the native PowerPC build. */
#include <CoreServices/CoreServices.h>
#include <stdio.h>
static void cf(const char *name, CFStringEncoding enc, const unsigned char *b, int n)
{
    CFStringRef s = CFStringCreateWithBytes(0, b, n, enc, false);
    char out[256] = "";
    UInt8 back[256];
    CFIndex used = 0;
    if (s) {
        CFStringGetCString(s, out, sizeof out, kCFStringEncodingUTF8);
        CFStringGetBytes(s, CFRangeMake(0, CFStringGetLength(s)), enc, '?', false, back, sizeof back, &used);
    }
    printf("CF %-10s: %s, length %ld, back %ld bytes %s\n", name, s ? out : "(failed)", s ? (long)CFStringGetLength(s) : -1L,
           (long)used, used == n && !memcmp(back, b, n) ? "same" : "different");
    if (s) CFRelease(s);
}
static void tec(const char *name, TextEncoding enc, const unsigned char *b, int n)
{
    TextToUnicodeInfo t2u;
    UnicodeToTextInfo u2t;
    UnicodeMapping m;
    UniChar u[128];
    unsigned char back[256];
    ByteCount rd = 0, len = 0, rd2 = 0, len2 = 0;
    OSStatus e1, e2 = -1;
    int k;
    m.unicodeEncoding = CreateTextEncoding(kTextEncodingUnicodeDefault, kTextEncodingDefaultVariant, kUnicode16BitFormat);
    m.otherEncoding = enc; m.mappingVersion = kUnicodeUseLatestMapping;
    e1 = CreateTextToUnicodeInfo(&m, &t2u);
    if (!e1) e1 = ConvertFromTextToUnicode(t2u, n, b, 0, 0, 0, 0, 0, sizeof u, &rd, &len, u);
    if (!e1 && !CreateUnicodeToTextInfo(&m, &u2t)) e2 = ConvertFromUnicodeToText(u2t, len, u, 0, 0, 0, 0, 0, sizeof back, &rd2, &len2, back);
    printf("TEC %-9s: %d, %lu UniChars:", name, (int)e1, (unsigned long)len / 2);
    for (k = 0; k < (int)(len / 2) && k < 6; k++) printf(" %04x", u[k]);
    printf(", back %d %s\n", (int)e2, len2 == (ByteCount)n && !memcmp(back, b, n) ? "same" : "different");
}
static void conv(const char *name, TextEncoding a, TextEncoding b, const void *in, ByteCount n)
{
    TECObjectRef t = 0;
    unsigned char out[128];
    ByteCount rd = 0, len = 0;
    OSStatus e = TECCreateConverter(&t, a, b), e2 = -1;
    int k;
    if (!e) { e2 = TECConvertText(t, (ConstTextPtr)in, n, &rd, out, sizeof out, &len); TECDisposeConverter(t); }
    printf("%-22s create %d convert %d, %lu bytes:", name, (int)e, (int)e2, (unsigned long)len);
    if ((b & 0xff00) == 0x100 && ((b >> 26) & 0x3f) == kUnicode16BitFormat) for (k = 0; k + 1 < (int)len && k < 16; k += 2) printf(" %04x", *(UInt16 *)(void *)(out + k));   /* units */
    else for (k = 0; k < (int)len && k < 16; k++) printf(" %02x", out[k]);
    printf("\n");
}

int main(void)
{
    static const unsigned char roman[] = { 0x8a, 0x9a, 0x9f, 0xa7, 'a' };           /* MacRoman: a-, o-, u-umlaut, sharp s, a */
    static const unsigned char sjis[] = { 0x93, 0xfa, 0x96, 0x7b, 0x8c, 0xea };     /* Shift-JIS: Nihongo */
    static const unsigned char gb[] = { 0xd6, 0xd0, 0xce, 0xc4 };                   /* GB 2312 (EUC-CN): Zhongwen */
    static const unsigned char kr[] = { 0xc7, 0xd1, 0xb1, 0xdb };                   /* EUC-KR: Hangul */
    cf("MacRoman", kCFStringEncodingMacRoman, roman, sizeof roman);
    cf("ShiftJIS", kCFStringEncodingShiftJIS, sjis, sizeof sjis);
    cf("GB2312", kCFStringEncodingGB_2312_80, gb, sizeof gb);
    cf("EUC-CN", kCFStringEncodingEUC_CN, gb, sizeof gb);
    cf("EUC-KR", kCFStringEncodingEUC_KR, kr, sizeof kr);
    tec("MacRoman", kTextEncodingMacRoman, roman, sizeof roman);
    tec("ShiftJIS", kTextEncodingShiftJIS, sjis, sizeof sjis);
    tec("EUC-CN", kTextEncodingEUC_CN, gb, sizeof gb);
    tec("EUC-KR", kTextEncodingEUC_KR, kr, sizeof kr);
    {
    TextEncoding u8 = CreateTextEncoding(kTextEncodingUnicodeDefault, kTextEncodingDefaultVariant, kUnicodeUTF8Format);
    TextEncoding u16 = CreateTextEncoding(kTextEncodingUnicodeDefault, kTextEncodingDefaultVariant, kUnicode16BitFormat);
    TextEncoding u32 = CreateTextEncoding(kTextEncodingUnicodeDefault, kTextEncodingDefaultVariant, kUnicode32BitFormat);
    static const char utf8[] = "a\xc3\xa4\xe6\x97\xa5";             /* a ae nichi */
    static const unsigned char roman2[] = { 'a', 0x8a };
    conv("UTF-8 -> UTF-32", u8, u32, utf8, 6);
    conv("UTF-8 -> UTF-16", u8, u16, utf8, 6);
    conv("MacRoman -> UTF-32", kTextEncodingMacRoman, u32, roman2, 2);
    conv("MacRoman -> UTF-8", kTextEncodingMacRoman, u8, roman2, 2);
    conv("UTF-8 -> MacRoman", u8, kTextEncodingMacRoman, utf8, 3);
    conv("UTF-8 -> ShiftJIS", u8, kTextEncodingShiftJIS, utf8 + 3, 3);
        }
    return 0;
}