/* macho.h - load the i386 part of a Mach-O file into the address space */
#ifndef MACHO_H
#define MACHO_H

#include <stdint.h>

typedef struct {
    uint32_t entrypoint;          /* eip from LC_UNIXTHREAD (slid) */
    uint32_t mh;                /* address of the Mach header (slid) */
    uint32_t slide;
    uint32_t from, bis;          /* occupied range */
    int has_dyld;
    char dyld[256];             /* LC_LOAD_DYLINKER */
    uint32_t filetype;          /* MH_EXECUTE, MH_DYLINKER, ... */
} Image;

/* loads the i386 version; slide is added to all segments. 0 = good, otherwise an error message has been printed. */
int macho_load(const char *path, uint32_t slide, Image *image);

/* host reserve for the guest: 0x1000 .. RESERVE_END (kept free via -pagezero_size) */
#define RESERVE_END 0x10000000u

#endif
