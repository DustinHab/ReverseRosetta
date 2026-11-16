/* quicktime.c - QuickTime through its own i386 framework (from Apple's QuickTime 7.6.4 for Leopard, see README):
 * open a movie, read duration, time scale, tracks, size and media types, draw frames into an offscreen GWorld and
 * report the mean of each colour channel. Codecs of the two builds may round differently, so the means are printed
 * coarsely (in steps of 4); the output must match the native PowerPC build. */
#include <QuickTime/QuickTime.h>
#include <stdio.h>
#include <stdlib.h>

static void frame(Movie m, GWorldPtr g, Ptr base, long rb, int w, int h, TimeValue t)
{
    unsigned long sum[3] = { 0, 0, 0 };
    int x, y;
    SetMovieTimeValue(m, t);
    UpdateMovie(m);
    MoviesTask(m, 0);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            const unsigned char *p = (const unsigned char *)base + y * rb + 4 * x;    /* ARGB in memory order */
            sum[0] += p[1]; sum[1] += p[2]; sum[2] += p[3];
        }
    printf("  frame at %ld: mean r %lu g %lu b %lu (steps of 4)\n", (long)t, sum[0] / (w * h) / 4, sum[1] / (w * h) / 4,
           sum[2] / (w * h) / 4);
    (void)g;
}

int main(int argc, char **argv)
{
    int i;
    if (EnterMovies() != noErr) { printf("EnterMovies failed\n"); return 1; }
    for (i = 1; i < argc; i++) {
        CFStringRef path = CFStringCreateWithCString(0, argv[i], kCFStringEncodingUTF8);
        Handle ref = 0;
        OSType type = 0;
        Movie m = 0;
        short id = 0;
        Rect box;
        long k, n;
        const char *name = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
        OSErr e = QTNewDataReferenceFromFullPathCFString(path, kQTNativeDefaultPathStyle, 0, &ref, &type);
        if (!e) e = NewMovieFromDataRef(&m, newMovieActive, &id, ref, type);
        if (e || !m) { printf("%s: error %d\n", name, e); continue; }
        GetMovieBox(m, &box);
        n = GetMovieTrackCount(m);
        printf("%s: duration %ld, time scale %ld, %ld tracks, box %d x %d\n", name, (long)GetMovieDuration(m),
               (long)GetMovieTimeScale(m), n, box.right - box.left, box.bottom - box.top);
        for (k = 1; k <= n; k++) {
            Track t = GetMovieIndTrack(m, k);
            OSType mt = 0;
            SampleDescriptionHandle sd = (SampleDescriptionHandle)NewHandle(0);
            OSType ct = 0;
            GetMediaHandlerDescription(GetTrackMedia(t), &mt, 0, 0);
            GetMediaSampleDescription(GetTrackMedia(t), 1, sd);
            if (GetHandleSize((Handle)sd) >= (Size)sizeof(SampleDescription)) ct = (**sd).dataFormat;
            DisposeHandle((Handle)sd);
            printf("  track %ld: media '%c%c%c%c', format '%c%c%c%c', duration %ld\n", k, (char)(mt >> 24), (char)(mt >> 16),
                   (char)(mt >> 8), (char)mt, (char)(ct >> 24), (char)(ct >> 16), (char)(ct >> 8), (char)ct,
                   (long)GetTrackDuration(t));
        }
        if (box.right > box.left && box.bottom > box.top && !getenv("QT_NO_FRAMES")) {
            int w = box.right - box.left, h = box.bottom - box.top;
            long rb = 4 * w;
            Ptr base = NewPtrClear(rb * h);
            GWorldPtr g = 0;
            Rect r = { 0, 0, 0, 0 };
            r.right = w; r.bottom = h;
            if (base && NewGWorldFromPtr(&g, k32ARGBPixelFormat, &r, 0, 0, 0, base, rb) == noErr) {
                TimeValue d = GetMovieDuration(m);
                SetMovieGWorld(m, g, 0);
                OffsetRect(&box, -box.left, -box.top);
                SetMovieBox(m, &box);
                frame(m, g, base, rb, w, h, 0);
                frame(m, g, base, rb, w, h, d / 2);
                DisposeGWorld(g);
            }
            if (base) DisposePtr(base);
        }
        DisposeMovie(m);
        DisposeHandle(ref);
        CFRelease(path);
    }
    ExitMovies();
    return 0;
}
