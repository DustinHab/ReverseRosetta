/* macho.c - load the i386 part of a Mach-O file (fat or thin).
 *
 * The host is built with "-pagezero_size 0x10000000": the lower 256 MB are reserved at startup (no access), so
 * neither malloc nor libraries of the host land there. Guest segments in this range get their place by freeing
 * that piece of the reserve and mapping the file there; elsewhere only if the range is free. Page 0
 * (0..0x1000) always stays without access.
 */
#include "macho.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <mach/mach.h>

#define CPU_I386 7
#define LC_SEGMENT_ 0x1
#define LC_UNIXTHREAD_ 0x5
#define LC_LOAD_DYLINKER_ 0xe

static uint32_t le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be(const uint8_t *p) { return (uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24; }

/* make space [a, a+n) free for the guest: free it in the reserve, elsewhere check that it is free */
static int slot(uint32_t a, uint32_t n)
{
    vm_address_t x = a;
    if (a < 0x1000) return -1;
    if ((uint64_t)a + n <= RESERVE_END) {
        vm_deallocate(mach_task_self(), a, n);
        return 0;
    }
    if (vm_allocate(mach_task_self(), &x, n, FALSE) != KERN_SUCCESS) return -1;
    vm_deallocate(mach_task_self(), x, n);
    return 0;
}

#define HEAD_FIRST 4096
#define HEAD_MAX (1u << 20)

/* The load commands can be longer than the first page (Safari, iCal: many libraries); then they are read again into
   a buffer of their size */
int macho_load(const char *path, uint32_t slide, Image *image)
{
    uint8_t first[HEAD_FIRST], *head = first, *lc;
    uint32_t part = 0, n, i, ncmds, size;
    int fd = open(path, O_RDONLY);
    memset(image, 0, sizeof *image);
    image->slide = slide;
    image->from = 0xffffffff;
    if (fd < 0) { fprintf(stderr, "rosetta: %s not readable\n", path); return -1; }
    if (pread(fd, head, HEAD_FIRST, 0) < 32) goto broken;
    if (be(head) == 0xcafebabe) {           /* fat: header in big endian */
        uint32_t k = be(head + 4);
        for (i = 0; i < k && 8 + 20 * (i + 1) <= HEAD_FIRST; i++)
            if (be(head + 8 + 20 * i) == CPU_I386) { part = be(head + 8 + 20 * i + 8); break; }
        if (i == k) { fprintf(stderr, "rosetta: %s has no Intel part (i386)\n", path); close(fd); return -1; }
        if (pread(fd, head, HEAD_FIRST, part) < 32) goto broken;
    }
    if (le(head) != 0xfeedface || le(head + 4) != CPU_I386) {
        fprintf(stderr, "rosetta: %s is not a 32-bit Intel program\n", path);
        goto fail;
    }
    image->filetype = le(head + 12);
    ncmds = le(head + 16);
    size = le(head + 20);
    if (size > HEAD_MAX) goto broken;
    if (28 + size > HEAD_FIRST) {
        if (!(head = malloc(28 + size)) || pread(fd, head, 28 + size, part) != (ssize_t)(28 + size)) goto broken;
    }
    lc = head + 28;
    for (n = 0; n < ncmds; n++) {
        uint32_t cmd = le(lc), cmdsize = le(lc + 4);
        if (cmdsize < 8 || lc + cmdsize > head + 28 + size) goto broken;
        if (cmd == LC_SEGMENT_) {
            uint32_t vmaddr = le(lc + 24), vmsize = le(lc + 28), fileoff = le(lc + 32), filesize = le(lc + 36);
            uint32_t prot = le(lc + 44), a = vmaddr + slide;
            char name[17];
            memcpy(name, lc + 8, 16); name[16] = 0;
            if (vmsize == 0 || (vmaddr == 0 && filesize == 0 && prot == 0)) { lc += cmdsize; continue; }  /* __PAGEZERO */
            prot = (prot & 7) | PROT_READ;
            if (slot(a, vmsize)) {
                fprintf(stderr, "rosetta: no space for %s %s at %08x (%u bytes)\n", path, name, a, vmsize);
                goto fail;
            }
            if (filesize) {
                uint32_t fileslice = (filesize + 4095) & ~4095u;
                if (fileslice > vmsize) fileslice = vmsize;
                if (mmap((void *)(uintptr_t)a, fileslice, (int)prot, MAP_FIXED | MAP_PRIVATE, fd,
                         (off_t)part + fileoff) == MAP_FAILED) {
                    perror("rosetta: mmap");
                    goto fail;
                }
                if (fileslice < vmsize &&
                    mmap((void *)(uintptr_t)(a + fileslice), vmsize - fileslice, (int)prot, MAP_FIXED | MAP_PRIVATE | MAP_ANON,
                         -1, 0) == MAP_FAILED) { perror("rosetta: mmap"); goto fail; }
                if (fileoff == 0) image->mh = a;
            } else if (mmap((void *)(uintptr_t)a, vmsize, (int)prot, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED) {
                perror("rosetta: mmap");
                goto fail;
            }
            if (a < image->from) image->from = a;
            if (a + vmsize > image->bis) image->bis = a + vmsize;
        } else if (cmd == LC_UNIXTHREAD_) {
            if (le(lc + 8) == 1)            /* i386_THREAD_STATE: eip is the 11th field */
                image->entrypoint = le(lc + 16 + 40) + slide;
        } else if (cmd == LC_LOAD_DYLINKER_) {
            uint32_t o = le(lc + 8);
            if (o < cmdsize) {
                image->has_dyld = 1;
                strncpy(image->dyld, (const char *)lc + o, sizeof image->dyld - 1);
            }
        }
        lc += cmdsize;
    }
    close(fd);
    if (head != first) free(head);
    if (!image->entrypoint) { fprintf(stderr, "rosetta: %s has no entry point\n", path); return -1; }
    return 0;
broken:
    fprintf(stderr, "rosetta: %s is corrupt or its header is too large\n", path);
fail:
    close(fd);
    if (head != first) free(head);
    return -1;
}
