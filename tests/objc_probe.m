/* objc_probe.m - query the Objective-C runtime (without NSLog): class chain, method lists, selector uniqueness */
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <string.h>

static void cls(const char *name, const char *sel)
{
    Class c = objc_getClass(name), k;
    unsigned n = 0, i, found = 0;
    Method *ml;
    SEL s = sel_registerName(sel);
    if (!c) { printf("%s: not found\n", name); return; }
    printf("%s:", name);
    for (k = c; k; k = class_getSuperclass(k)) printf(" %s", class_getName(k));
    ml = class_copyMethodList(c, &n);
    for (i = 0; i < n; i++) if (method_getName(ml[i]) == s) found = 1;
    printf(" | %u methods, %s %s, by name %s\n", n, sel, class_getInstanceMethod(c, s) ? "yes" : "NO",
           found ? "yes" : "no");
    for (i = 0; i < n && i < 3; i++) printf("   %s\n", sel_getName(method_getName(ml[i])));
    free(ml);
}

int main(void)
{
    SEL a = @selector(UTF8String), b = sel_registerName("UTF8String");
    printf("selector UTF8String: %s (%s)\n", a == b ? "unique" : "DUPLICATE", sel_getName(a));
    cls("NSObject", "description");
    cls("NSString", "UTF8String");
    cls("NSCFString", "UTF8String");
    cls("NSRunLoop", "_init:");
    cls("NSArray", "count");
    return 0;
}
