/* mmap_cost.c - how expensive the steps are with which dyld maps a library (measured natively).
 * mmap_cost FILE [LENGTH_HEX] [ADDRESS_HEX]
 * a) vm_allocate fixed + mmap MAP_FIXED over it (this is how dyld does it), b) only mmap MAP_FIXED, c) mmap without
 * a fixed address, d) open + stat of the same file. 50 times each, mean in microseconds. */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <mach/mach.h>
#include <mach/mach_time.h>

static double us(uint64_t d)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)d * tb.numer / tb.denom / 1000.0;
}

int main(int argc, char **argv)
{
    int fd, i, N = 50;
    struct stat st;
    vm_size_t len;
    vm_address_t addr;
    uint64_t t, sa = 0, sm = 0, sb = 0, sc = 0, so = 0, ss = 0, su = 0, sr = 0;
    if (argc < 2 || (fd = open(argv[1], O_RDONLY)) < 0 || fstat(fd, &st)) { fprintf(stderr, "file?\n"); return 1; }
    len = argc > 2 ? strtoul(argv[2], 0, 16) : (vm_size_t)((st.st_size + 4095) & ~4095);
    addr = argc > 3 ? strtoul(argv[3], 0, 16) : 0x44285000u;
    for (i = 0; i < N; i++) {
        vm_address_t a = addr;
        void *r;
        t = mach_absolute_time();
        if (vm_allocate(mach_task_self(), &a, len, FALSE)) { fprintf(stderr, "vm_allocate %08lx occupied\n", (unsigned long)addr); return 1; }
        sa += mach_absolute_time() - t;
        t = mach_absolute_time();
        r = mmap((void *)addr, len, PROT_READ | PROT_EXEC, MAP_FIXED | MAP_PRIVATE, fd, 0);
        sm += mach_absolute_time() - t;
        if (r == MAP_FAILED) { perror("mmap"); return 1; }
        t = mach_absolute_time();
        munmap((void *)addr, len);
        su += mach_absolute_time() - t;
        t = mach_absolute_time();
        r = mmap((void *)addr, len, PROT_READ | PROT_EXEC, MAP_FIXED | MAP_PRIVATE, fd, 0);
        sb += mach_absolute_time() - t;
        munmap((void *)addr, len);
        t = mach_absolute_time();
        r = mmap(0, len, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
        sc += mach_absolute_time() - t;
        munmap(r, len);
        t = mach_absolute_time();
        r = mmap((void *)addr, len, PROT_READ, MAP_FIXED | MAP_SHARED, fd, 0);
        sr += mach_absolute_time() - t;
        munmap((void *)addr, len);
        t = mach_absolute_time();
        { int f2 = open(argv[1], O_RDONLY); so += mach_absolute_time() - t; close(f2); }
        t = mach_absolute_time();
        { struct stat s2; stat(argv[1], &s2); ss += mach_absolute_time() - t; }
    }
    printf("length %lu KB: vm_allocate %.1f, mmap over it %.1f, munmap %.1f | only MAP_FIXED %.1f | anywhere %.1f | MAP_SHARED %.1f | open %.1f, stat %.1f us\n",
           (unsigned long)len / 1024, us(sa) / N, us(sm) / N, us(su) / N, us(sb) / N, us(sc) / N, us(sr) / N, us(so) / N, us(ss) / N);
    return 0;
}
