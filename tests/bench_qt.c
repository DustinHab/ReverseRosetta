/* bench_qt.c - QuickTime decoding speed: step through every video frame of the movies (nextTimeMediaSample) and draw
 * it into an offscreen GWorld. Prints frames and a checksum on stdout (the same as the native PowerPC build, up to
 * codec rounding) and the time of the decoding loop alone on stderr, without opening the movies and loading codecs. */
#include <QuickTime/QuickTime.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    int i, rounds = getenv("QT_ROUNDS") ? atoi(getenv("QT_ROUNDS")) : 1;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    if (EnterMovies() != noErr) { printf("EnterMovies failed\n"); return 1; }
    for (i = 1; i < argc; i++) {
        CFStringRef path = CFStringCreateWithCString(0, argv[i], kCFStringEncodingUTF8);
        const char *name = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
        Handle ref = 0;
        OSType type = 0, video = VisualMediaCharacteristic;
        Movie m = 0;
        short id = 0;
        Rect box;
        int w, h, k;
        long rb, frames = 0;
        Ptr base;
        GWorldPtr g = 0;
        unsigned long sum = 0;
        uint64_t t0, t1;
        OSErr e = QTNewDataReferenceFromFullPathCFString(path, kQTNativeDefaultPathStyle, 0, &ref, &type);
        if (!e) e = NewMovieFromDataRef(&m, newMovieActive, &id, ref, type);
        if (e || !m) { printf("%s: error %d\n", name, e); continue; }
        GetMovieBox(m, &box);
        w = box.right - box.left; h = box.bottom - box.top;
        if (w <= 0 || h <= 0) { printf("%s: no picture\n", name); continue; }
        rb = 4 * w;
        base = NewPtrClear(rb * h);
        {
            Rect r = { 0, 0, 0, 0 };
            r.right = w; r.bottom = h;
            if (!base || NewGWorldFromPtr(&g, k32ARGBPixelFormat, &r, 0, 0, 0, base, rb) != noErr) {
                printf("%s: no GWorld\n", name); continue;
            }
        }
        SetMovieGWorld(m, g, 0);
        OffsetRect(&box, -box.left, -box.top);
        SetMovieBox(m, &box);
        SetMovieTimeValue(m, 0);
        UpdateMovie(m);
        MoviesTask(m, 0);                     /* first frame: codec set up outside the measurement */
        t0 = mach_absolute_time();
        for (k = 0; k < rounds; k++) {
            TimeValue t = 0, next = 0;
            SetMovieTimeValue(m, 0);
            for (;;) {
                long y;
                UpdateMovie(m);
                MoviesTask(m, 0);
                frames++;
                for (y = 0; y < h; y += 7) sum += (unsigned char)base[y * rb + 4 * (y % w) + 2];
                GetMovieNextInterestingTime(m, nextTimeMediaSample, 1, &video, t, fixed1, &next, 0);
                if (next < 0 || next <= t) break;
                t = next;
                SetMovieTimeValue(m, t);
            }
        }
        t1 = mach_absolute_time();
        printf("%s: %d x %d, %ld frames, checksum %lu\n", name, w, h, frames, sum / 64);
        fprintf(stderr, "%s: %.1f ms, %.2f ms per frame\n", name, (double)(t1 - t0) * tb.numer / tb.denom / 1e6,
                (double)(t1 - t0) * tb.numer / tb.denom / 1e6 / (double)frames);
        DisposeMovie(m);
        DisposeGWorld(g);
    }
    return 0;
}
