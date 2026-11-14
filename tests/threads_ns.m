/* threads_ns.m - Foundation with threads: NSThread, NSLock, NSCondition, NSOperationQueue, autorelease per thread;
 * plus a spin loop with pause that waits for another thread (without a system call). */
#import <Foundation/Foundation.h>
#include <pthread.h>

#if defined(__i386__)
#define WAIT() __asm__ volatile("pause")
#else
#define WAIT() do { } while (0)
#endif

static NSLock *g_lock;
static NSCondition *g_cond;
static long g_n;
static int g_done;
static volatile int g_flag;

@interface Work : NSObject
- (void)pass:(NSNumber *)nr;
- (void)op:(NSNumber *)nr;
@end

@implementation Work
- (void)pass:(NSNumber *)nr
{
    NSAutoreleasePool *p = [[NSAutoreleasePool alloc] init];
    NSMutableString *s = [NSMutableString string];
    int i;
    for (i = 0; i < 3000; i++) {
        [g_lock lock];
        g_n++;
        [g_lock unlock];
        if (i % 1000 == 0) [s appendFormat:@"%d.", i];
    }
    [g_cond lock];
    g_done++;
    [g_cond signal];
    [g_cond unlock];
    if (![s isEqualToString:@"0.1000.2000."]) printf("wrong in thread %d\n", [nr intValue]);
    [p release];
}
- (void)op:(NSNumber *)nr
{
    int i, k = [nr intValue];
    long sum = 0;
    for (i = 0; i < 20000; i++) sum += (i ^ k) & 15;
    [g_lock lock];
    g_n += sum;
    [g_lock unlock];
}
@end

static void *setter(void *a)
{
    (void)a;
    usleep(2000);
    g_flag = 1;
    return 0;
}

int main(void)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    Work *a = [[Work alloc] init];
    NSOperationQueue *q;
    pthread_t t;
    int i;
    long before;

    g_lock = [[NSLock alloc] init];
    g_cond = [[NSCondition alloc] init];
    for (i = 0; i < 4; i++)
        [NSThread detachNewThreadSelector:@selector(pass:) toTarget:a withObject:[NSNumber numberWithInt:i]];
    [g_cond lock];
    while (g_done < 4) [g_cond wait];
    [g_cond unlock];
    printf("NSThread: n = %ld, multithreaded %d\n", g_n, (int)[NSThread isMultiThreaded]);

    before = g_n;
    q = [[NSOperationQueue alloc] init];
    [q setMaxConcurrentOperationCount:2];
    for (i = 0; i < 8; i++)
        [q addOperation:[[[NSInvocationOperation alloc] initWithTarget:a selector:@selector(op:)
                                                                 object:[NSNumber numberWithInt:i]] autorelease]];
    [q waitUntilAllOperationsAreFinished];
    printf("NSOperationQueue: sum %ld\n", g_n - before);

    pthread_create(&t, 0, setter, 0);       /* spin loop with pause, without a system call */
    while (!g_flag) WAIT();
    pthread_join(t, 0);
    printf("spin loop finished\n");

    [q release];
    [a release];
    [pool release];
    return 0;
}
