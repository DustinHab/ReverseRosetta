/* kern_exec.c - what the kernel answers to sysctl {CTL_KERN, 44 (KERN_TRANSLATE), pid} and {CTL_KERN, 45 (KERN_EXEC)} */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/sysctl.h>

int main(void)
{
    int mib[3];
    char buf[256];
    size_t l = sizeof buf;
    int r;
    mib[0] = CTL_KERN; mib[1] = 45;
    memset(buf, 0, sizeof buf);
    r = sysctl(mib, 2, buf, &l, 0, 0);
    printf("kern.exec: r %d errno %d length %lu content \"%s\" first byte %d\n", r, r ? errno : 0, (unsigned long)l, buf, buf[0]);
    mib[1] = 44; mib[2] = getpid(); l = sizeof buf;
    memset(buf, 0, sizeof buf);
    r = sysctl(mib, 3, buf, &l, 0, 0);
    printf("kern.translate.pid: r %d errno %d length %lu value %d\n", r, r ? errno : 0, (unsigned long)l, *(int *)buf);
    return 0;
}
