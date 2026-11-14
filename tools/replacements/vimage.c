/* vimage.c - replacement for Intel's vImage (missing in the 10.5.8 Combo Update): the 15 functions that i386
 * libraries of the Intel root import (HIServices, ImageIO, CoreGraphics filters, QuartzCore, QuartzComposer,
 * RawCamera), in C. Built as i386 and translated by rosetta like any Intel code. Premultiplication exactly like
 * vImage ((c * a + 127) / 255), bilinear scaling (vImage: Lanczos), convolutions with edge extension or background
 * color. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <Accelerate/Accelerate.h>

#define LINE(b, y) ((uint8_t *)(b)->data + (size_t)(y) * (b)->rowBytes)

static int valid(const vImage_Buffer *b) { return b && b->data && b->width && b->height; }

void SetvImageVectorAvailable(int an) { (void)an; }

vImage_Error vImagePremultiplyData_ARGB8888(const vImage_Buffer *src, const vImage_Buffer *dest, vImage_Flags flags)
{
    vImagePixelCount x, y;
    (void)flags;
    if (!valid(src) || !valid(dest)) return kvImageNullPointerArgument;
    for (y = 0; y < dest->height && y < src->height; y++) {
        const uint8_t *s = LINE(src, y);
        uint8_t *d = LINE(dest, y);
        for (x = 0; x < dest->width && x < src->width; x++, s += 4, d += 4) {
            unsigned a = s[0];
            d[0] = (uint8_t)a;
            d[1] = (uint8_t)((s[1] * a + 127) / 255);
            d[2] = (uint8_t)((s[2] * a + 127) / 255);
            d[3] = (uint8_t)((s[3] * a + 127) / 255);
        }
    }
    return kvImageNoError;
}

vImage_Error vImageUnpremultiplyData_ARGB8888(const vImage_Buffer *src, const vImage_Buffer *dest, vImage_Flags flags)
{
    vImagePixelCount x, y;
    int k;
    (void)flags;
    if (!valid(src) || !valid(dest)) return kvImageNullPointerArgument;
    for (y = 0; y < dest->height && y < src->height; y++) {
        const uint8_t *s = LINE(src, y);
        uint8_t *d = LINE(dest, y);
        for (x = 0; x < dest->width && x < src->width; x++, s += 4, d += 4) {
            unsigned a = s[0];
            d[0] = (uint8_t)a;
            for (k = 1; k < 4; k++) {
                unsigned v = a ? (s[k] * 255u + a / 2) / a : 0;
                d[k] = (uint8_t)(v > 255 ? 255 : v);
            }
        }
    }
    return kvImageNoError;
}

vImage_Error vImageOverwriteChannelsWithScalar_ARGB8888(Pixel_8 scalar, const vImage_Buffer *src, const vImage_Buffer *dest,
                                                        uint8_t copyMask, vImage_Flags flags)
{
    vImagePixelCount x, y;
    int k;
    (void)flags;
    if (!valid(src) || !valid(dest)) return kvImageNullPointerArgument;
    for (y = 0; y < dest->height; y++) {
        const uint8_t *s = LINE(src, y);
        uint8_t *d = LINE(dest, y);
        for (x = 0; x < dest->width; x++, s += 4, d += 4)
            for (k = 0; k < 4; k++) d[k] = (copyMask >> (3 - k) & 1) ? scalar : s[k];   /* mask: 8 = A, 4 = R, 2 = G, 1 = B */
    }
    return kvImageNoError;
}

vImage_Error vImageTableLookUp_Planar8(const vImage_Buffer *src, const vImage_Buffer *dest, const Pixel_8 table[256],
                                       vImage_Flags flags)
{
    vImagePixelCount x, y;
    (void)flags;
    if (!valid(src) || !valid(dest) || !table) return kvImageNullPointerArgument;
    for (y = 0; y < dest->height; y++) {
        const uint8_t *s = LINE(src, y);
        uint8_t *d = LINE(dest, y);
        for (x = 0; x < dest->width; x++) d[x] = table[s[x]];
    }
    return kvImageNoError;
}

vImage_Error vImageConvert_PlanarFtoPlanar8(const vImage_Buffer *src, const vImage_Buffer *dest, Pixel_F maxFloat,
                                            Pixel_F minFloat, vImage_Flags flags)
{
    vImagePixelCount x, y;
    float f = maxFloat > minFloat ? 255.0f / (maxFloat - minFloat) : 0.0f;
    (void)flags;
    if (!valid(src) || !valid(dest)) return kvImageNullPointerArgument;
    for (y = 0; y < dest->height; y++) {
        const float *s = (const float *)LINE(src, y);
        uint8_t *d = LINE(dest, y);
        for (x = 0; x < dest->width; x++) {
            float v = (s[x] - minFloat) * f + 0.5f;
            d[x] = (uint8_t)(v <= 0 ? 0 : v >= 255 ? 255 : (int)v);
        }
    }
    return kvImageNoError;
}

vImage_Error vImageConvert_FTo16U(const vImage_Buffer *src, const vImage_Buffer *dest, float offset, float scale,
                                  vImage_Flags flags)
{
    vImagePixelCount x, y;
    (void)flags;
    if (!valid(src) || !valid(dest) || scale == 0) return kvImageNullPointerArgument;
    for (y = 0; y < dest->height; y++) {
        const float *s = (const float *)LINE(src, y);
        uint16_t *d = (uint16_t *)LINE(dest, y);
        for (x = 0; x < dest->width; x++) {
            float v = (s[x] - offset) / scale + 0.5f;
            d[x] = (uint16_t)(v <= 0 ? 0 : v >= 65535 ? 65535 : (int)v);
        }
    }
    return kvImageNoError;
}

vImage_Error vImageHistogramCalculation_Planar8(const vImage_Buffer *src, vImagePixelCount *histogram, vImage_Flags flags)
{
    vImagePixelCount x, y;
    (void)flags;
    if (!valid(src) || !histogram) return kvImageNullPointerArgument;
    memset(histogram, 0, 256 * sizeof *histogram);
    for (y = 0; y < src->height; y++) {
        const uint8_t *s = LINE(src, y);
        for (x = 0; x < src->width; x++) histogram[s[x]]++;
    }
    return kvImageNoError;
}

vImage_Error vImageHistogramCalculation_ARGB8888(const vImage_Buffer *src, vImagePixelCount *histogram[4], vImage_Flags flags)
{
    vImagePixelCount x, y;
    int k;
    (void)flags;
    if (!valid(src) || !histogram) return kvImageNullPointerArgument;
    for (k = 0; k < 4; k++) memset(histogram[k], 0, 256 * sizeof *histogram[k]);
    for (y = 0; y < src->height; y++) {
        const uint8_t *s = LINE(src, y);
        for (x = 0; x < src->width; x++, s += 4) for (k = 0; k < 4; k++) histogram[k][s[k]]++;
    }
    return kvImageNoError;
}

static unsigned bin_of(float v, unsigned n, float lo, float hi)
{
    float t = hi > lo ? (v - lo) / (hi - lo) * (float)n : 0;
    if (!(t > 0)) return 0;                     /* also NaN */
    return t >= (float)n ? n - 1 : (unsigned)t;
}

vImage_Error vImageHistogramCalculation_PlanarF(const vImage_Buffer *src, vImagePixelCount *histogram,
                                                unsigned int histogram_entries, Pixel_F minVal, Pixel_F maxVal,
                                                vImage_Flags flags)
{
    vImagePixelCount x, y;
    (void)flags;
    if (!valid(src) || !histogram || !histogram_entries) return kvImageNullPointerArgument;
    memset(histogram, 0, histogram_entries * sizeof *histogram);
    for (y = 0; y < src->height; y++) {
        const float *s = (const float *)LINE(src, y);
        for (x = 0; x < src->width; x++) histogram[bin_of(s[x], histogram_entries, minVal, maxVal)]++;
    }
    return kvImageNoError;
}

vImage_Error vImageHistogramCalculation_ARGBFFFF(const vImage_Buffer *src, vImagePixelCount *histogram[4],
                                                 unsigned int histogram_entries, Pixel_F minVal, Pixel_F maxVal,
                                                 vImage_Flags flags)
{
    vImagePixelCount x, y;
    int k;
    (void)flags;
    if (!valid(src) || !histogram || !histogram_entries) return kvImageNullPointerArgument;
    for (k = 0; k < 4; k++) memset(histogram[k], 0, histogram_entries * sizeof *histogram[k]);
    for (y = 0; y < src->height; y++) {
        const float *s = (const float *)LINE(src, y);
        for (x = 0; x < src->width; x++, s += 4)
            for (k = 0; k < 4; k++) histogram[k][bin_of(s[k], histogram_entries, minVal, maxVal)]++;
    }
    return kvImageNoError;
}

/* bilinear, pixel centers mapped onto each other, edge extended; n channels per pixel */
static vImage_Error rescale(const vImage_Buffer *src, const vImage_Buffer *dest, int n, vImage_Flags flags)
{
    vImagePixelCount x, y;
    double fx, fy;
    int k;
    if (flags & kvImageGetTempBufferSize) return 0;
    if (!valid(src) || !valid(dest)) return kvImageNullPointerArgument;
    fx = (double)src->width / (double)dest->width;
    fy = (double)src->height / (double)dest->height;
    for (y = 0; y < dest->height; y++) {
        double sy = (y + 0.5) * fy - 0.5;
        long y0 = (long)floor(sy), y1 = y0 + 1;
        double wy = sy - (double)y0;
        uint8_t *d = LINE(dest, y);
        if (y0 < 0) y0 = 0;
        if (y1 < 0) y1 = 0;
        if (y0 >= (long)src->height) y0 = (long)src->height - 1;
        if (y1 >= (long)src->height) y1 = (long)src->height - 1;
        for (x = 0; x < dest->width; x++) {
            double sx = (x + 0.5) * fx - 0.5;
            long x0 = (long)floor(sx), x1 = x0 + 1;
            double wx = sx - (double)x0;
            if (x0 < 0) x0 = 0;
            if (x1 < 0) x1 = 0;
            if (x0 >= (long)src->width) x0 = (long)src->width - 1;
            if (x1 >= (long)src->width) x1 = (long)src->width - 1;
            for (k = 0; k < n; k++) {
                double a = LINE(src, y0)[n * x0 + k], b = LINE(src, y0)[n * x1 + k];
                double c = LINE(src, y1)[n * x0 + k], e = LINE(src, y1)[n * x1 + k];
                double v = (a * (1 - wx) + b * wx) * (1 - wy) + (c * (1 - wx) + e * wx) * wy + 0.5;
                d[n * x + k] = (uint8_t)(v >= 255 ? 255 : v <= 0 ? 0 : (int)v);
            }
        }
    }
    return kvImageNoError;
}

vImage_Error vImageScale_ARGB8888(const vImage_Buffer *src, const vImage_Buffer *dest, void *tempBuffer, vImage_Flags flags)
{
    (void)tempBuffer;
    return rescale(src, dest, 4, flags);
}

vImage_Error vImageScale_Planar8(const vImage_Buffer *src, const vImage_Buffer *dest, void *tempBuffer, vImage_Flags flags)
{
    (void)tempBuffer;
    return rescale(src, dest, 1, flags);
}

/* source pixel (x, y) with edge handling: extend or background (0: outside) */
static const uint8_t *src(const vImage_Buffer *src, long x, long y, int n, vImage_Flags flags)
{
    if (x < 0 || y < 0 || x >= (long)src->width || y >= (long)src->height) {
        if (!(flags & kvImageEdgeExtend)) return 0;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= (long)src->width) x = (long)src->width - 1;
        if (y >= (long)src->height) y = (long)src->height - 1;
    }
    return LINE(src, y) + n * x;
}

vImage_Error vImageConvolve_Planar8(const vImage_Buffer *src, const vImage_Buffer *dest, void *tempBuffer,
                                    vImagePixelCount srcOffsetToROI_X, vImagePixelCount srcOffsetToROI_Y,
                                    const int16_t *kernel, uint32_t kernel_height, uint32_t kernel_width, int32_t divisor,
                                    Pixel_8 backgroundColor, vImage_Flags flags)
{
    vImagePixelCount x, y;
    uint32_t i, j;
    (void)tempBuffer;
    if (flags & kvImageGetTempBufferSize) return 0;
    if (!valid(src) || !valid(dest) || !kernel || !(kernel_height & 1) || !(kernel_width & 1)) return kvImageInvalidKernelSize;
    if (!divisor) divisor = 1;
    for (y = 0; y < dest->height; y++) {
        uint8_t *d = LINE(dest, y);
        for (x = 0; x < dest->width; x++) {
            long s = 0, v;
            for (i = 0; i < kernel_height; i++)
                for (j = 0; j < kernel_width; j++) {
                    const uint8_t *q = src(src, (long)(x + srcOffsetToROI_X + j) - kernel_width / 2,
                                              (long)(y + srcOffsetToROI_Y + i) - kernel_height / 2, 1, flags);
                    s += (long)kernel[i * kernel_width + j] * (q ? *q : backgroundColor);
                }
            v = (s + (s >= 0 ? divisor / 2 : -divisor / 2)) / divisor;
            d[x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
    return kvImageNoError;
}

vImage_Error vImageTentConvolve_ARGB8888(const vImage_Buffer *src, const vImage_Buffer *dest, void *tempBuffer,
                                         vImagePixelCount srcOffsetToROI_X, vImagePixelCount srcOffsetToROI_Y,
                                         uint32_t kernel_height, uint32_t kernel_width, Pixel_8888 backgroundColor,
                                         vImage_Flags flags)
{
    vImagePixelCount x, y;
    long i, j, hh = kernel_height / 2, hb = kernel_width / 2;
    int k;
    (void)tempBuffer;
    if (flags & kvImageGetTempBufferSize) return 0;
    if (!valid(src) || !valid(dest) || !(kernel_height & 1) || !(kernel_width & 1)) return kvImageInvalidKernelSize;
    for (y = 0; y < dest->height; y++) {
        uint8_t *d = LINE(dest, y);
        for (x = 0; x < dest->width; x++) {
            long s[4] = { 0, 0, 0, 0 }, sum = 0;
            for (i = -hh; i <= hh; i++)
                for (j = -hb; j <= hb; j++) {
                    long w = (hh + 1 - labs(i)) * (hb + 1 - labs(j));
                    const uint8_t *q = src(src, (long)(x + srcOffsetToROI_X) + j, (long)(y + srcOffsetToROI_Y) + i, 4, flags);
                    for (k = 0; k < 4; k++) s[k] += w * (q ? q[k] : backgroundColor[k]);
                    sum += w;
                }
            for (k = 0; k < 4; k++) d[4 * x + k] = (uint8_t)((s[k] + sum / 2) / sum);
        }
    }
    return kvImageNoError;
}
