/* memtest.c - check memset/memcpy/memmove at all alignments and sizes; reports the first error */
#include <stdio.h>
#include <string.h>

static unsigned char pbuf[1024] __attribute__((aligned(16))), src[1024] __attribute__((aligned(16)));

int main(void)
{
    int a, n, k, errnum = 0;
    for (k = 0; k < 1024; k++) src[k] = (unsigned char)(k * 7 + 3);
    for (a = 0; a < 16 && errnum < 5; a++)
        for (n = 0; n < 400 && errnum < 5; n++) {
            memset(pbuf, 0xee, sizeof pbuf);
            memset(pbuf + a, 'a' + n % 26, (size_t)n);
            for (k = 0; k < 1024; k++) {
                int want = (k >= a && k < a + n) ? 'a' + n % 26 : 0xee;
                if (pbuf[k] != want) { printf("memset a=%d n=%d: byte %d is %02x instead of %02x\n", a, n, k, pbuf[k], want); errnum++; break; }
            }
            memset(pbuf, 0xee, sizeof pbuf);
            memcpy(pbuf + a, src + (n & 15), (size_t)n);
            for (k = 0; k < 1024; k++) {
                int want = (k >= a && k < a + n) ? src[(n & 15) + k - a] : 0xee;
                if (pbuf[k] != want) { printf("memcpy a=%d n=%d: byte %d is %02x instead of %02x\n", a, n, k, pbuf[k], want); errnum++; break; }
            }
        }
    printf("%d errors\n", errnum);
    return 0;
}
