/* window_list.c PID - windows of a process (only this process) according to the WindowServer: number, level,
 * rectangle (top left, points). CGWindowListCopyWindowInfo returns nothing on this Leopard; therefore via the
 * private CGS interface: process -> PSN -> connection of the process -> its visible windows.
 * Build: gcc-4.2 -o window_list window_list.c -framework ApplicationServices -framework Carbon */
#include <stdio.h>
#include <stdlib.h>
#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>

typedef int CGSConnectionID;
extern CGSConnectionID _CGSDefaultConnection(void);
extern CGError CGSGetConnectionIDForPSN(CGSConnectionID cid, const ProcessSerialNumber *psn, CGSConnectionID *target);
extern CGError CGSGetOnScreenWindowCount(CGSConnectionID cid, CGSConnectionID target, int *n);
extern CGError CGSGetOnScreenWindowList(CGSConnectionID cid, CGSConnectionID target, int max, int *plist, int *n);
extern CGError CGSGetWindowBounds(CGSConnectionID cid, int wid, CGRect *r);
extern CGError CGSGetWindowLevel(CGSConnectionID cid, int wid, int *level);

int main(int argc, char **argv)
{
    ProcessSerialNumber psn;
    CGSConnectionID cid = _CGSDefaultConnection(), target = 0;
    int plist[64], n = 0, i;
    if (argc < 2) { fprintf(stderr, "window_list PID\n"); return 2; }
    if (GetProcessForPID(atoi(argv[1]), &psn)) { printf("no program (PSN) for %s\n", argv[1]); return 1; }
    if (CGSGetConnectionIDForPSN(cid, &psn, &target) || !target) { printf("no WindowServer connection\n"); return 1; }
    if (CGSGetOnScreenWindowList(cid, target, 64, plist, &n)) { printf("no window list\n"); return 1; }
    printf("%d visible windows\n", n);
    for (i = 0; i < n; i++) {
        CGRect r;
        int level = 0;
        CGSGetWindowLevel(cid, plist[i], &level);
        if (!CGSGetWindowBounds(cid, plist[i], &r))
            printf("WINDOW %d level %d at %.0f %.0f %.0f %.0f\n", plist[i], level, r.origin.x, r.origin.y, r.size.width,
                   r.size.height);
    }
    return 0;
}
