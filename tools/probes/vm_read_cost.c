/* vm_read_cost.c - what a safe read of 16 bytes costs on the G4: vm_read_overwrite (a Mach call) against a copy
 * guarded by setjmp (the translator's read probe, jit.c safe_read). Build: gcc-4.2 -O2 -o vm_read_cost vm_read_cost.c */
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <mach/mach.h>
#include <mach/mach_time.h>

static double us(uint64_t t)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return (double)t * tb.numer / tb.denom / 1e3;
}

int main(void)
{
    static unsigned char src[4096], dst[16];
    jmp_buf jb;
    int i, n = 100000;
    uint64_t t0, t1, t2;
    vm_size_t got;
    memset(src, 7, sizeof src);
    t0 = mach_absolute_time();
    for (i = 0; i < n; i++)
        vm_read_overwrite(mach_task_self(), (vm_address_t)(src + (i & 255) * 16), 16, (vm_address_t)dst, &got);
    t1 = mach_absolute_time();
    for (i = 0; i < n; i++)
        if (!_setjmp(jb)) memcpy(dst, src + (i & 255) * 16, 16);
    t2 = mach_absolute_time();
    printf("vm_read_overwrite: %.2f us per read; setjmp + copy: %.3f us per read\n", us(t1 - t0) / n, us(t2 - t1) / n);
    return 0;
}
