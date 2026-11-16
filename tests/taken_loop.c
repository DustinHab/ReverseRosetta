/* taken_loop.c - loops that run over the taken branch of a conditional jump (the rarely needed part is inline after
 * the jump): the translator follows the taken branch so that the loop is one block, the fall-through becomes the side
 * exit with the condition inverted. A hash chain search like gzip's longest_match, skipped long bodies with signed and
 * unsigned compares of every kind, and a body that is entered now and then. Translated (ROSETTA_THRESHOLD=1, also
 * ROSETTA_NO_TAKEN=1) against the interpreter (ROSETTA_INTERP=1). */
#include <stdio.h>

#define WSIZE 0x8000
static unsigned char win[2 * WSIZE + 300];
static unsigned short prev[WSIZE];
static unsigned int seed = 12345;

static unsigned int rnd(void) { seed = seed * 1103515245u + 12345u; return seed >> 16; }

/* gzip's longest_match: most chain entries fail at the first byte compare */
static unsigned int __attribute__((noinline)) longest(unsigned int cur, unsigned int scan, unsigned int limit, int chain)
{
    unsigned int best = 2, best_at = 0;
    unsigned char e1 = win[scan + best - 1], e0 = win[scan + best];
    do {
        const unsigned char *m = win + cur, *s = win + scan;
        unsigned int len;
        if (m[best] != e0 || m[best - 1] != e1 || m[0] != s[0] || m[1] != s[1]) continue;
        len = 2;
        while (len < 258 && m[len] == s[len]) len++;
        if (len > best) {
            best = len; best_at = cur;
            if (len >= 258) break;
            e1 = s[best - 1]; e0 = s[best];
        }
    } while ((cur = prev[cur & (WSIZE - 1)]) > limit && --chain != 0);
    return best * 65536 + best_at;
}

/* a long body skipped by most iterations, with the comparison kind k */
static int __attribute__((noinline)) skipped(const int *a, int n, int k, int t)
{
    int i, acc = 0, hits = 0;
    for (i = 0; i < n; i++) {
        int v = a[i], c;
        switch (k) {
        case 0: c = v > t; break;
        case 1: c = v < -t; break;
        case 2: c = (unsigned int)v > (unsigned int)t * 3; break;
        case 3: c = (unsigned int)v < 7u; break;
        case 4: c = (v & 0xff) == 0x5a; break;
        default: c = v >= t && v <= t + 100; break;
        }
        if (__builtin_expect(c, 0)) {
            int j;
            hits++;
            for (j = 0; j < 8; j++) acc += (v >> j) ^ (acc << 3) ^ (i * j);
            acc ^= v * 2654435761u;
            acc += hits << (i & 7);
            if (acc < 0) acc = -acc;
            acc %= 1000003;
        }
        acc += v & 3;
    }
    return acc * 31 + hits;
}

int main(void)
{
    static int a[20000];
    unsigned int i, sum = 0;
    int k;
    for (i = 0; i < sizeof win; i++) win[i] = (unsigned char)("abcabcabdaab"[rnd() % 12]);
    for (i = 0; i < WSIZE; i++) prev[i] = (unsigned short)(i > 40 ? i - 1 - rnd() % 40 : 0);
    for (i = 1000; i < 1000 + 4000; i += 7) sum = sum * 33 + longest(i + WSIZE / 2, i, 600, 64 + (int)(i % 50));
    printf("longest_match %08x\n", sum);
    for (i = 0; i < 20000; i++) a[i] = (int)(rnd() * 37u) - 900000;
    for (k = 0; k < 6; k++) printf("skipped %d: %d\n", k, skipped(a, 20000, k, 1200000));
    return 0;
}
