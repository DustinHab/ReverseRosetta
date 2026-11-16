/* objc_classes.c LIBRARY - (PowerPC, built by tools/stub_classes.sh) the Objective-C classes that a library of this
 * system defines, with their superclass and the library that defines it: one line "class<TAB>superclass<TAB>image".
 * The PowerPC framework of the same path knows the class hierarchy that its i386 counterpart (missing from the
 * 10.5.8 update) would have; stub libraries define these classes with the right superclass. */
#include <objc/runtime.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    int n, i;
    Class *cl;
    if (argc < 2) { fprintf(stderr, "usage: objc_classes LIBRARY\n"); return 2; }
    dlopen("/System/Library/Frameworks/Cocoa.framework/Cocoa", RTLD_LAZY | RTLD_GLOBAL);
    if (!dlopen(argv[1], RTLD_LAZY | RTLD_GLOBAL)) { fprintf(stderr, "objc_classes: %s\n", dlerror()); return 1; }
    n = objc_getClassList(0, 0);
    cl = malloc(sizeof *cl * (size_t)n);
    if (!cl) return 1;
    n = objc_getClassList(cl, n);
    for (i = 0; i < n; i++) {
        const char *img = class_getImageName(cl[i]);
        Class s;
        if (!img || strcmp(img, argv[1])) continue;
        s = class_getSuperclass(cl[i]);
        printf("%s\t%s\t%s\n", class_getName(cl[i]), s ? class_getName(s) : "", s && class_getImageName(s) ? class_getImageName(s) : "");
    }
    return 0;
}
