/* conditions.c - compares, tests and jumps in all sizes and signednesses. The output depends only on the
   C semantics: the PowerPC version of the same program provides the reference. */
#include <stdio.h>

static volatile unsigned vals[] = {
    0, 1, 2, 3, 0x7f, 0x80, 0x81, 0xff, 0x100, 0x7fff, 0x8000, 0x8001, 0xffff, 0x10000, 0x12345,
    0x7fffffff, 0x80000000, 0x80000001, 0xfffffffe, 0xffffffff, 12345, 0xdeadbeef, 0xfffffff0, 0xff80
};
#define N (sizeof vals / sizeof vals[0])

static unsigned h;
static void m(unsigned x) { h = h * 31 + x; }

static unsigned tally(unsigned x)
{
    unsigned n = 0;
    while (x) { x &= x - 1; n++; }
    return n;
}

static int loop(int n)
{
    int s = 0;
    while (--n >= 0) s += n & 3 ? n : -n;
    return s;
}

int main(void)
{
    unsigned i, j;
    for (i = 0; i < N; i++)
        for (j = 0; j < N; j++) {
            unsigned a = vals[i], b = vals[j];
            int sa = (int)a, sb = (int)b;
            signed char ca = (signed char)a, cb = (signed char)b;
            unsigned char ua = (unsigned char)a, ub = (unsigned char)b;
            short wa = (short)a, wb = (short)b;
            unsigned short uwa = (unsigned short)a, uwb = (unsigned short)b;
            m(a < b); m(a <= b); m(a > b); m(a >= b); m(a == b); m(a != b);
            m(sa < sb); m(sa <= sb); m(sa > sb); m(sa >= sb);
            m(ca < cb); m(ca > cb); m(ca <= cb); m(ua < ub); m(ua >= ub); m(ca == cb);
            m(wa < wb); m(wa >= wb); m(uwa < uwb); m(uwa > uwb); m(wa == wb);
            m((a & b) == 0); m((int)(a & b) < 0); m((int)(a & b) <= 0); m((int)(a | b) > 0); m((a ^ b) == 0);
            m((int)(a - b) < 0); m(a - b == 0); m((int)(a + b) < 0); m(a + b == 0);
            m((ua & ub) != 0); m((signed char)(ua & ub) < 0); m((short)(uwa & uwb) < 0);
            m(a + 1 == 0); m((int)(a - 1) < 0); m(a - 1 == 0);
            if (sa < 5) m(1);
            if (a > 0x80) m(2);
            if ((a & 0x100) != 0) m(3);
            if ((a & 0x10000) == 0) m(4);
            if ((signed char)a < 3) m(5);
            if ((a & 0xff0000) == 0x10000) m(6);
            if ((short)a > -2) m(7);
            if ((a & 0x80000000u) != 0) m(8);
            if ((ua & 0x80) != 0) m(9);
            m(sa < -3); m(a < 0x12345); m(sa > 70000); m(sa >= -70000); m(a <= 0xffff8000u);
            m(sa > sb ? sa - sb : sb - sa);
            m(a < b ? a : b);
            m(ca > cb ? (unsigned)ca : (unsigned)cb);
        }
    for (i = 0; i < N; i++) m(tally(vals[i]));
    m((unsigned)loop(1000));
    printf("%08x\n", h);
    return 0;
}
