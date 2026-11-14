/* foundation.m - Foundation and Objective-C (Intel versions from the Combo Update): messages, strings, arrays,
   dictionaries, autorelease pool, exceptions */
#import <Foundation/Foundation.h>
#include <stdio.h>

@interface Counter : NSObject { int n; }
- (void)plus:(int)k;
- (int)val;
@end

@implementation Counter
- (void)plus:(int)k { n += k; }
- (int)val { return n; }
@end

int main(void)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSString *s = [NSString stringWithFormat:@"Hello %d %@", 42, @"World"];
    NSArray *a = [NSArray arrayWithObjects:@"pear", @"apple", @"quince", nil];
    NSArray *sorted = [a sortedArrayUsingSelector:@selector(compare:)];
    NSMutableDictionary *d = [NSMutableDictionary dictionary];
    Counter *z = [[Counter alloc] init];
    int i;
    for (i = 0; i < 50; i++) [d setObject:[NSNumber numberWithInt:i * i] forKey:[NSString stringWithFormat:@"k%d", i]];
    for (i = 1; i <= 10; i++) [z plus:i];
    printf("%s | %s | %d | %lu | %d\n", [s UTF8String], [[sorted componentsJoinedByString:@","] UTF8String],
           [[d objectForKey:@"k7"] intValue], (unsigned long)[d count], [z val]);
    @try {
        [a objectAtIndex:10];
    } @catch (NSException *e) {
        printf("exception: %s\n", [[e name] UTF8String]);
    }
    [z release];
    [pool release];
    return 0;
}
