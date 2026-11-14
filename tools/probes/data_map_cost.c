/* data_map_cost.c - map a private data segment of a large, resident file and measure the cost (native).
 * data_map_cost FILE OFFSET_HEX LENGTH_HEX ADDRESS_HEX
 * a) mmap MAP_PRIVATE from the file, b) anonymous + pread, c) MAP_PRIVATE from a copy of only this range (separate
 * file, open included). Each followed by writing every page once (like dyld/initializers). Mean of 30, us. */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <mach/mach_time.h>

static double us(uint64_t d)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)d * tb.numer / tb.denom / 1000.0;
}

static uint64_t poke(char *a, size_t n)
{
    uint64_t t = mach_absolute_time();
    size_t i;
    for (i = 0; i < n; i += 4096) a[i]++;
    return mach_absolute_time() - t;
}

int main(int argc, char **argv)
{
    int fd, fk, i, N = 30;
    off_t off;
    size_t len;
    char *addr, *buf;
    uint64_t t, ma = 0, ba = 0, mb = 0, bb = 0, mc = 0, bc = 0;
    const char *copy = "/tmp/data_copy.bin";
    if (argc < 5 || (fd = open(argv[1], O_RDONLY)) < 0) { fprintf(stderr, "data_map_cost FILE OFFSET LENGTH ADDRESS\n"); return 1; }
    off = (off_t)strtoul(argv[2], 0, 16);
    len = strtoul(argv[3], 0, 16);
    addr = (char *)strtoul(argv[4], 0, 16);
    buf = malloc(len);
    pread(fd, buf, len, off);
    fk = open(copy, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    write(fk, buf, len);
    close(fk);
    for (i = 0; i < N; i++) {
        void *r;
        t = mach_absolute_time();
        r = mmap(addr, len, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE, fd, off);
        ma += mach_absolute_time() - t;
        if (r == MAP_FAILED) { perror("mmap a"); return 1; }
        ba += poke(addr, len);
        munmap(addr, len);
        t = mach_absolute_time();
        r = mmap(addr, len, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
        pread(fd, addr, len, off);
        mb += mach_absolute_time() - t;
        bb += poke(addr, len);
        munmap(addr, len);
        t = mach_absolute_time();
        fk = open(copy, O_RDONLY);
        r = mmap(addr, len, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE, fk, 0);
        close(fk);
        mc += mach_absolute_time() - t;
        if (r == MAP_FAILED) { perror("mmap c"); return 1; }
        bc += poke(addr, len);
        munmap(addr, len);
    }
    printf("%lu KB: a) file private %.1f + write %.1f | b) anonymous+pread %.1f + %.1f | c) copy private %.1f + %.1f us\n",
           (unsigned long)len / 1024, us(ma) / N, us(ba) / N, us(mb) / N, us(bb) / N, us(mc) / N, us(bc) / N);
    return 0;
}
