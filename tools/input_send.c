/* input_send.c - send real input events into the login session (for tests and screenshots of translated programs,
 * whose windows the accessibility API does not see):
 *   input_send click X Y      left click at screen position X, Y (activates the window there)
 *   input_send type TEXT      type TEXT as key events (characters, independent of the keyboard layout)
 *   input_send keys CODE...   press keys by virtual key code
 * Must run inside the login session (tests/in_session.sh). */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void click(double x, double y)
{
    CGPoint p = CGPointMake(x, y);
    CGEventRef e = CGEventCreateMouseEvent(0, kCGEventMouseMoved, p, kCGMouseButtonLeft);
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
    usleep(50000);
    e = CGEventCreateMouseEvent(0, kCGEventLeftMouseDown, p, kCGMouseButtonLeft);
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
    usleep(50000);
    e = CGEventCreateMouseEvent(0, kCGEventLeftMouseUp, p, kCGMouseButtonLeft);
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
}

static void type(const char *s)
{
    for (; *s; s++) {
        UniChar u = (UniChar)(unsigned char)*s;
        CGKeyCode code = *s == '\n' ? 36 : 0;     /* return; otherwise the character decides */
        CGEventRef down = CGEventCreateKeyboardEvent(0, code, true), up = CGEventCreateKeyboardEvent(0, code, false);
        if (*s != '\n') { CGEventKeyboardSetUnicodeString(down, 1, &u); CGEventKeyboardSetUnicodeString(up, 1, &u); }
        CGEventPost(kCGHIDEventTap, down);
        CGEventPost(kCGHIDEventTap, up);
        CFRelease(down);
        CFRelease(up);
        usleep(120000);                         /* translated programs need a moment per key */
    }
}

/* raw virtual key codes (as a real keyboard sends them; keypad: 82-92 digits, 67 *, 69 +, 78 -, 75 /, 81 =);
   "cmd+12" is command-Q */
static void keys(int n, char **codes)
{
    int k;
    for (k = 0; k < n; k++) {
        const char *s = codes[k];
        CGEventFlags flags = 0;
        CGKeyCode code;
        CGEventRef down, up;
        if (!strncmp(s, "cmd+", 4)) { flags = kCGEventFlagMaskCommand; s += 4; }
        code = (CGKeyCode)atoi(s);
        down = CGEventCreateKeyboardEvent(0, code, true);
        up = CGEventCreateKeyboardEvent(0, code, false);
        if (flags) { CGEventSetFlags(down, flags); CGEventSetFlags(up, flags); }
        CGEventPost(kCGHIDEventTap, down);
        CGEventPost(kCGHIDEventTap, up);
        CFRelease(down);
        CFRelease(up);
        usleep(150000);
    }
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "click")) click(atof(argv[2]), atof(argv[3]));
    else if (argc == 3 && !strcmp(argv[1], "type")) type(argv[2]);
    else if (argc >= 3 && !strcmp(argv[1], "keys")) keys(argc - 2, argv + 2);
    else { fprintf(stderr, "usage: input_send click X Y | input_send type TEXT | input_send keys CODE...\n"); return 2; }
    return 0;
}
