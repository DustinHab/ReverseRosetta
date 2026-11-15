/* jpeg_decode.c - time decoding an image with ImageIO and drawing it into a bitmap (as a browser does), to compare
 * the translated i386 build with the native one:
 *   gcc -arch i386 -o jpeg_decode_i386 jpeg_decode.c -framework ApplicationServices
 *   gcc -o jpeg_decode jpeg_decode.c -framework ApplicationServices
 *   jpeg_decode FILE [REPEAT] */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

static double now(void)
{
    struct timeval t;
    gettimeofday(&t, 0);
    return t.tv_sec + t.tv_usec / 1e6;
}

int main(int argc, char **argv)
{
    int i, n = argc > 2 ? atoi(argv[2]) : 1;
    if (argc < 2) { fprintf(stderr, "usage: jpeg_decode FILE [REPEAT]\n"); return 2; }
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(0, (const UInt8 *)argv[1], strlen(argv[1]), false);
    for (i = 0; i < n; i++) {
        double t0 = now(), t1, t2;
        CGImageSourceRef src = CGImageSourceCreateWithURL(url, 0);
        CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, 0) : 0;
        if (!img) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
        size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        void *pix = calloc(w * h, 4);
        CGContextRef ctx = CGBitmapContextCreate(pix, w, h, 8, w * 4, cs, kCGImageAlphaPremultipliedFirst);
        t1 = now();
        CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);   /* decodes here */
        t2 = now();
        {
            unsigned long sum = 0;
            size_t k;
            for (k = 0; k < w * h * 4; k += 4099) sum += ((unsigned char *)pix)[k];
            printf("%zux%zu: open %.2f s, decode+draw %.2f s, checksum %lu\n", w, h, t1 - t0, t2 - t1, sum);
        }
        CGContextRelease(ctx);
        free(pix);
        CGColorSpaceRelease(cs);
        CGImageRelease(img);
        CFRelease(src);
    }
    return 0;
}
