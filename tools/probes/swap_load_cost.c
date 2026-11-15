/* swap_load_cost.c - cost of getting little-endian doubles into FPRs on the G4 (native PowerPC, gcc-4.2 -O2):
 * per iteration two doubles, the shape of the prefetch ring (jit.c, "Hoisting array accesses"):
 *   A  4 lwz  + 4 stw + 2 lfd            (no byte swap: the reference)
 *   B  4 lwbrx + 4 stw + 2 lfd           (what the translator emits now)
 *   C  2 lvx + 2 vperm + 2 stvx + 2 lfd  (AltiVec swap of the 16 bytes around each double)
 *   D  1 lvx + 1 vperm + 1 stvx + 2 lfd  (contiguous stream: one vector for two doubles)
 * Prints nanoseconds per iteration (fastest of 7 runs). */
#include <stdio.h>
#include <stdint.h>
#include <mach/mach_time.h>

#define N 4096
static double src[N + 64] __attribute__((aligned(16)));
static double ring[64] __attribute__((aligned(128)));
static const unsigned char swap8[16] __attribute__((aligned(16))) = { 7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8 };

static double run(int kind)
{
    double s = 0;
    const double *p = src;
    double *r = ring;
    int i;
    for (i = 0; i < N; i += 2, p += 2) {
        double x, y;
        switch (kind) {
        case 0:
            __asm__ volatile("lwz r3,0(%2)\n lwz r4,4(%2)\n lwz r5,8(%2)\n lwz r6,12(%2)\n"
                             "stw r3,0(%3)\n stw r4,4(%3)\n stw r5,8(%3)\n stw r6,12(%3)\n lfd %0,32(%3)\n lfd %1,40(%3)"
                             : "=f"(x), "=f"(y) : "b"(p), "b"(r) : "r3", "r4", "r5", "r6", "memory");
            break;
        case 1:
            __asm__ volatile("li r0,4\n lwbrx r3,%2,r0\n lwbrx r4,0,%2\n addi r7,%2,8\n lwbrx r5,r7,r0\n lwbrx r6,0,r7\n"
                             "stw r3,0(%3)\n stw r4,4(%3)\n stw r5,8(%3)\n stw r6,12(%3)\n lfd %0,32(%3)\n lfd %1,40(%3)"
                             : "=f"(x), "=f"(y) : "b"(p), "b"(r) : "r0", "r3", "r4", "r5", "r6", "r7", "memory");
            break;
        case 2:
            __asm__ volatile("lvx v2,0,%4\n lvx v0,0,%2\n vperm v0,v0,v0,v2\n stvx v0,0,%3\n"
                             "li r0,16\n lvx v1,%2,r0\n vperm v1,v1,v1,v2\n stvx v1,%3,r0\n lfd %0,32(%3)\n lfd %1,48(%3)"
                             : "=f"(x), "=f"(y) : "b"(p), "b"(r), "b"(swap8) : "r0", "v0", "v1", "v2", "memory");
            break;
        default:
            __asm__ volatile("lvx v2,0,%4\n lvx v0,0,%2\n vperm v0,v0,v0,v2\n stvx v0,0,%3\n lfd %0,32(%3)\n lfd %1,40(%3)"
                             : "=f"(x), "=f"(y) : "b"(p), "b"(r), "b"(swap8) : "v0", "v2", "memory");
        }
        s += x + y;
        r = r + 8 < ring + 64 ? r + 8 : ring;
    }
    return s;
}

int main(void)
{
    static const char *const names[] = { "A 4 lwz + 4 stw + 2 lfd", "B 4 lwbrx + 4 stw + 2 lfd",
                                         "C 2 lvx/vperm/stvx + 2 lfd", "D 1 lvx/vperm/stvx + 2 lfd" };
    mach_timebase_info_data_t tb;
    int k, t;
    double sink = 0;
    mach_timebase_info(&tb);
    for (k = 0; k < N; k++) src[k] = k;
    for (k = 0; k < 4; k++) {
        double best = 1e30;
        for (t = 0; t < 7; t++) {
            uint64_t t0 = mach_absolute_time();
            sink += run(k);
            double ns = (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / (N / 2);
            if (ns < best) best = ns;
        }
        printf("%-30s %.2f ns per iteration (two doubles) = %.1f cycles at 1.33 GHz\n", names[k], best, best * 1.333);
    }
    return sink == 12345.0;
}
