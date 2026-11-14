/* cf.c - CoreFoundation (Intel version from the Combo Update): strings, arrays, dictionary, numbers, sorting */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>

static void show(const char *name, CFStringRef s)
{
    char t[256];
    if (!CFStringGetCString(s, t, sizeof t, kCFStringEncodingUTF8)) t[0] = 0;
    printf("%s: %s\n", name, t);
}

int main(void)
{
    CFStringRef s = CFStringCreateWithFormat(0, 0, CFSTR("Hello %d, %.3f, %@"), 42, 3.14159, CFSTR("World"));
    CFMutableArrayRef a = CFArrayCreateMutable(0, 0, &kCFTypeArrayCallBacks);
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(0, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef w;
    int i, sum = 0;
    show("format", s);
    CFArrayAppendValue(a, CFSTR("pear"));
    CFArrayAppendValue(a, CFSTR("apple"));
    CFArrayAppendValue(a, CFSTR("quince"));
    CFArraySortValues(a, CFRangeMake(0, CFArrayGetCount(a)), (CFComparatorFunction)CFStringCompare, 0);
    w = CFStringCreateByCombiningStrings(0, a, CFSTR(","));
    show("sorted", w);
    for (i = 0; i < 100; i++) {
        CFNumberRef n = CFNumberCreate(0, kCFNumberIntType, &i);
        CFStringRef k = CFStringCreateWithFormat(0, 0, CFSTR("k%d"), i);
        CFDictionarySetValue(d, k, n);
        CFRelease(n);
        CFRelease(k);
    }
    for (i = 0; i < 100; i += 7) {
        CFStringRef k = CFStringCreateWithFormat(0, 0, CFSTR("k%d"), i);
        CFNumberRef n = CFDictionaryGetValue(d, k);
        int v = 0;
        if (n) CFNumberGetValue(n, kCFNumberIntType, &v);
        sum += v;
        CFRelease(k);
    }
    printf("dictionary %ld entries, sum %d\n", (long)CFDictionaryGetCount(d), sum);
    printf("length %ld, uppercase: ", (long)CFStringGetLength(s));
    {
        CFMutableStringRef m = CFStringCreateMutableCopy(0, 0, s);
        CFStringUppercase(m, 0);
        show("", m);
        CFRelease(m);
    }
    CFRelease(w); CFRelease(a); CFRelease(d); CFRelease(s);
    return 0;
}
