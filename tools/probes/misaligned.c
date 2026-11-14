/* misaligned.c - cost of misaligned accesses on the G4 (native): lwbrx/stwbrx/lwz/stw at offset
 * 0..3 and across a cache line boundary. 100000 accesses each, nanoseconds per access. */
#include <stdio.h>
#include <stdint.h>
#include <mach/mach_time.h>

static uint8_t g_buffer[8192] __attribute__((aligned(4096)));

static double ns(uint64_t d, int n)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)d * tb.numer / tb.denom / n;
}

int main(void)
{
    int v, i, N = 100000;
    for (v = 0; v < 5; v++) {
        uint32_t off = v < 4 ? 64 + (uint32_t)v : 30;     /* 30: across the 32-byte line */
        uint8_t *p = g_buffer + off;
        uint32_t x = 0x12345678, s = 0;
        uint64_t t;
        t = mach_absolute_time();
        for (i = 0; i < N; i++) __asm__ volatile("stwbrx %0,0,%1" : : "r"(x + i), "r"(p) : "memory");
        printf("offset %2u: stwbrx %6.1f", off, ns(mach_absolute_time() - t, N));
        t = mach_absolute_time();
        for (i = 0; i < N; i++) { uint32_t r; __asm__ volatile("lwbrx %0,0,%1" : "=r"(r) : "r"(p) : "memory"); s += r; }
        printf("  lwbrx %6.1f", ns(mach_absolute_time() - t, N));
        t = mach_absolute_time();
        for (i = 0; i < N; i++) __asm__ volatile("stw %0,0(%1)" : : "r"(x + i), "r"(p) : "memory");
        printf("  stw %6.1f", ns(mach_absolute_time() - t, N));
        t = mach_absolute_time();
        for (i = 0; i < N; i++) { uint32_t r; __asm__ volatile("lwz %0,0(%1)" : "=r"(r) : "r"(p) : "memory"); s += r; }
        printf("  lwz %6.1f ns (%u)\n", ns(mach_absolute_time() - t, N), s & 1);
    }
    return 0;
}
