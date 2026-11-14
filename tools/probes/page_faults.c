/* page_faults.c - what the first write to fresh pages costs on the G4 (calloc, vm_allocate RWX, mlock beforehand).
   gcc -O2 -o page_faults page_faults.c && ./page_faults */
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <mach/mach.h>

static double now(void)
{
    struct timeval t;
    gettimeofday(&t, 0);
    return t.tv_sec + t.tv_usec * 1e-6;
}

static double touch(volatile char *p, size_t n)
{
    double t0 = now();
    size_t i;
    for (i = 0; i < n; i += 4096) p[i] = 1;
    return (now() - t0) * 1e6 / (double)(n / 4096);
}

int main(void)
{
    size_t n = 4u << 20;
    vm_address_t a = 0;
    char *c;
    double t0;
    c = calloc(1, n);
    printf("calloc:            %.1f us per page\n", touch(c, n));
    vm_allocate(mach_task_self(), &a, n, TRUE);
    mprotect((void *)a, n, PROT_READ | PROT_WRITE | PROT_EXEC);
    printf("vm_allocate RWX:   %.1f us per page\n", touch((char *)a, n));
    a = 0;
    vm_allocate(mach_task_self(), &a, n, TRUE);
    t0 = now();
    mlock((void *)a, n);
    printf("mlock beforehand:  %.1f us per page", (now() - t0) * 1e6 / (double)(n / 4096));
    printf(", afterwards %.1f us\n", touch((char *)a, n));
    return 0;
}
