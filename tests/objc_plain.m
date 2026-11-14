/* objc_plain.m - Objective-C without Foundation (only libobjc, base class Object): classes, messages, categories */
#include <objc/Object.h>
#include <objc/runtime.h>
#include <stdio.h>

@interface Num : Object { int n; }
- (int)n;
- (id)setN:(int)k;
@end
@implementation Num
- (int)n { return n; }
- (id)setN:(int)k { n = k; return self; }
@end

@interface Num (Double)
- (int)dbl;
@end
@implementation Num (Double)
- (int)dbl { return 2 * [self n]; }
@end

int main(void)
{
    Num *z;
    unsigned m = 0;
    Method *ml;
    z = [[Num alloc] init];
    [z setN:21];
    ml = class_copyMethodList(objc_getClass("Num"), &m);
    {
        unsigned i;
        printf("%u methods:", m);
        for (i = 0; i < m; i++) printf(" %s", sel_getName(method_getName(ml[i])));
        printf("; selector %s\n", @selector(dbl) == sel_registerName("dbl") ? "unique" : "DUPLICATE");
    }
    fflush(stdout);
    printf("n %d, double %d\n", [z n], [z dbl]);
    free(ml);
    [z free];
    return 0;
}
