/* fsref_dump.c - the 80 bytes of an FSRef for the same files, built natively (PowerPC) and translated (i386): shows
 * which fields the File Manager keeps and in which byte order, for converting FSRefs that the PowerPC font server
 * sends to Intel clients (ATS GetFSRefForFileToken). Build: gcc-4.2 -arch ppc -arch i386 -framework CoreServices */
#include <stdio.h>
#include <CoreServices/CoreServices.h>

int main(int argc, char **argv)
{
    const char *paths[] = { "/System/Library/Fonts/LucidaGrande.dfont", "/Library/Fonts/Arial", "/tmp", "/" };
    int i, k;
    for (i = 0; i < 4; i++) {
        FSRef r;
        OSStatus e = FSPathMakeRef((const UInt8 *)(argc > 1 && i == 0 ? argv[1] : paths[i]), &r, 0);
        printf("%-42s %d:", argc > 1 && i == 0 ? argv[1] : paths[i], (int)e);
        for (k = 0; k < 80; k++) printf("%s%02x", k % 4 ? "" : " ", r.hidden[k]);
        printf("\n");
    }
    return 0;
}
