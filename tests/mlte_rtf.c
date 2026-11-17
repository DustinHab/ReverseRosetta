/* mlte_rtf.c - RTF through MLTE (Carbon's text engine), which reads and writes it with its plug-in MLTEFile.bundle:
 * a styled RTF file into an HITextView (TXNSetDataFromCFURLRef), text and style runs read back, the document flattened
 * to RTF (TXNFlattenObjectToCFDataRef), written to a file and read into a second view. Built for i386 (translated:
 * tools/intel_replacements.sh builds the plug-in into the Intel root) and natively; the output must be the same. */
#include <Carbon/Carbon.h>
#include <stdio.h>
#include <string.h>

static const char rtf[] =
    "{\\rtf1\\ansi\\ansicpg1252\\cocoartf949\n{\\fonttbl\\f0\\fswiss\\fcharset0 Helvetica;\\f1\\froman\\fcharset0 Times-Roman;}\n"
    "{\\colortbl;\\red255\\green255\\blue255;\\red200\\green20\\blue20;}\n"
    "\\pard\\qc\\f0\\b\\fs32 Licence\\b0\\fs24 \\\n\\pard\\ql\\f1\\i Please read\\i0  this \\cf2 agreement\\cf0  "
    "carefully.\\\nSecond line, \\ul underlined\\ulnone .}\n";

static void show(TXNObject obj, const char *what)
{
    Handle h = 0;
    ItemCount runs = 0, k;
    char buf[200];
    if (TXNGetDataEncoded(obj, kTXNStartOffset, kTXNEndOffset, &h, kTXNUnicodeTextData) == noErr && h) {
        UniChar *u = (UniChar *)*h;
        long n = GetHandleSize(h) / 2, i;
        for (i = 0; i < n && i < 120; i++) buf[i] = u[i] < 128 ? (char)(u[i] == '\r' || u[i] == '\n' ? '|' : u[i]) : '?';
        buf[i] = 0;
        printf("%s: %ld characters \"%s\"\n", what, n, buf);
        DisposeHandle(h);
    } else printf("%s: no text\n", what);
    TXNCountRunsInRange(obj, kTXNStartOffset, kTXNEndOffset, &runs);
    printf("%s: %lu style runs\n", what, (unsigned long)runs);
    for (k = 0; k < runs && k < 12; k++) {
        TXNOffset a = 0, b = 0;
        TXNDataType type = 0;
        TXNTypeAttributes attr[3];
        RGBColor col = { 0, 0, 0 };
        attr[0].tag = kTXNQDFontSizeAttribute; attr[0].size = kTXNFontSizeAttributeSize; attr[0].data.dataValue = 0;
        attr[1].tag = kTXNQDFontColorAttribute; attr[1].size = kTXNQDFontColorAttributeSize; attr[1].data.dataPtr = &col;
        attr[2].tag = kTXNQDFontStyleAttribute; attr[2].size = kTXNQDFontStyleAttributeSize; attr[2].data.dataValue = 0;
        if (TXNGetIndexedRunInfoFromRange(obj, k, kTXNStartOffset, kTXNEndOffset, &a, &b, &type, 3, attr) == noErr)
            printf("  %lu-%lu size %d face %d color %d/%d/%d\n", (unsigned long)a, (unsigned long)b,
                   (int)((Fixed)attr[0].data.dataValue >> 16), (int)attr[2].data.dataValue,
                   col.red >> 8, col.green >> 8, col.blue >> 8);
    }
}

static TXNObject load(const char *path, const char *what)
{
    HIViewRef view = 0;
    TXNObject obj;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(0, (const UInt8 *)path, (CFIndex)strlen(path), false);
    OSStatus err = HITextViewCreate(0, 0, 0, &view);
    if (err || !view) { printf("%s: HITextViewCreate %d\n", what, (int)err); return 0; }
    obj = HITextViewGetTXNObject(view);
    err = TXNSetDataFromCFURLRef(obj, url, kTXNStartOffset, kTXNEndOffset);
    printf("%s: TXNSetDataFromCFURLRef %d\n", what, (int)err);
    CFRelease(url);
    show(obj, what);
    return obj;
}

int main(void)
{
    const char *path = "/tmp/mlte_rtf_test.rtf", *again = "/tmp/mlte_rtf_flat.rtf";
    FILE *f = fopen(path, "w");
    TXNObject obj;
    CFDataRef flat = 0;
    OSStatus err;
    if (!f) return 1;
    fputs(rtf, f);
    fclose(f);
    if (!(obj = load(path, "read"))) return 1;
    err = TXNFlattenObjectToCFDataRef(obj, kTXNRichTextFormatData, &flat);
    printf("flatten to RTF: %d, %s\n", (int)err, flat && CFDataGetLength(flat) > 100 ? "data" : "no data");
    if (flat && (f = fopen(again, "w"))) {
        fwrite(CFDataGetBytePtr(flat), 1, (size_t)CFDataGetLength(flat), f);
        fclose(f);
        load(again, "again");
    }
    if (flat) CFRelease(flat);
    return 0;
}
