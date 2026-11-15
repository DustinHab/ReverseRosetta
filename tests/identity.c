/* identity.c - the processor an Intel program sees: an Intel Core Duo with SSE3, like CPUID and the commpage
 * (compared against tests/identity.expected; i386 only) */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <mach-o/arch.h>

static void show_int(const char *name)
{
    int v = -1;
    size_t l = sizeof v;
    if (sysctlbyname(name, &v, &l, 0, 0) < 0) printf("%s: %s\n", name, strerror(errno));
    else printf("%s: %d\n", name, v);
}

int main(void)
{
    const NXArchInfo *a = NXGetLocalArchInfo();
    char s[128];
    size_t l = sizeof s;
    int order = 0, mib[2] = { CTL_HW, HW_BYTEORDER };
    printf("NXGetLocalArchInfo: %s (type %d, subtype %d)\n", a ? a->name : "-", a ? a->cputype : -1, a ? a->cpusubtype : -1);
    show_int("hw.cputype");
    show_int("hw.cpusubtype");
    show_int("hw.optional.sse");
    show_int("hw.optional.sse2");
    show_int("hw.optional.sse3");
    show_int("hw.optional.supplementalsse3");
    show_int("hw.optional.x86_64");
    show_int("hw.optional.altivec");
    if (sysctlbyname("machdep.cpu.vendor", s, &l, 0, 0) == 0) printf("machdep.cpu.vendor: %s\n", s);
    l = sizeof order;
    sysctl(mib, 2, &order, &l, 0, 0);
    printf("hw.byteorder: %d\n", order);
    l = sizeof s;
    mib[1] = HW_MACHINE;
    sysctl(mib, 2, s, &l, 0, 0);
    printf("hw.machine: %s\n", s);
    return 0;
}
