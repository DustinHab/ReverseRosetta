/* remap.c - code at a fixed address is replaced: another file mapped over it (MAP_FIXED), then made writable and
 * overwritten. A caller elsewhere (its own file mapping, a direct call) has to see the new code each time, also when
 * the translator inlined the old one into its block. run_tests.sh starts it twice, so the second run starts with the
 * translation cache of the first. The files stay in /tmp unchanged between runs (same inode, same time) and start
 * with a minimal Mach-O header, like libraries (only code in images goes into the translation cache). "remap N"
 * only maps file N and calls, then ends with everything still mapped: run with 1 and then 2 on one translation
 * cache, the second run finds the caller translated with the code of file 1 inlined.
 * Compared against tests/remap.expected; i386 only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define CODE 0x0e000000u                /* the code that changes (in the guest reserve below 0x10000000) */
#define CALLER 0x0e100000u              /* call CODE; add $100, %eax; ret */
#define ENTRY 0x100                     /* code behind the header */

static const unsigned char ret1[] = { 0xb8, 1, 0, 0, 0, 0xc3 };     /* mov $1, %eax; ret */
static const unsigned char ret2[] = { 0xb8, 2, 0, 0, 0, 0xc3 };
static const unsigned char ret3[] = { 0xb8, 3, 0, 0, 0, 0xc3 };
static const unsigned char ret4[] = { 0xb8, 4, 0, 0, 0, 0xc3 };

/* one page: Mach-O header of a bundle with one segment __TEXT over the page, the code at ENTRY, the rest int3;
   rewritten only if the content differs */
static void file_make(const char *path, const unsigned char *code, size_t n)
{
    unsigned char page[4096], old[4096];
    uint32_t head[7] = { 0xfeedface, 7, 3, 8, 1, 56, 0 };
    uint32_t seg[14] = { 1, 56, 0, 0, 0, 0, 0, 0x1000, 0, 0x1000, 5, 5, 0, 0 };
    char tmp[256];
    int fd;
    memset(page, 0xcc, sizeof page);
    memcpy((char *)&seg[2], "__TEXT", 6);
    memcpy(page, head, sizeof head);
    memcpy(page + sizeof head, seg, sizeof seg);
    memcpy(page + ENTRY, code, n);
    if ((fd = open(path, O_RDONLY)) >= 0) {
        ssize_t r = read(fd, old, sizeof old);
        close(fd);
        if (r == (ssize_t)sizeof old && !memcmp(old, page, sizeof page)) return;
    }
    snprintf(tmp, sizeof tmp, "%s.%d", path, (int)getpid());
    if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0 || write(fd, page, sizeof page) != (ssize_t)sizeof page) {
        perror(tmp);
        exit(1);
    }
    close(fd);
    rename(tmp, path);
}

static void map_at(unsigned at, const char *path)
{
    int fd = open(path, O_RDONLY);
    void *r = fd < 0 ? MAP_FAILED : mmap((void *)at, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_FIXED, fd, 0);
    if (r != (void *)at) { perror(path); exit(1); }
    close(fd);
}

static void calls(const char *label)
{
    int (*caller)(void) = (int (*)(void))(CALLER + ENTRY);
    int i, s = 0, v = 0;
    for (i = 0; i < 1000; i++) s += v = caller();
    printf("%s: %d (sum %d)\n", label, v, s);
}

int main(int argc, char **argv)
{
    unsigned char call[16];
    int rel = (int)((CODE + ENTRY) - (CALLER + ENTRY + 5));
    call[0] = 0xe8;
    memcpy(call + 1, &rel, 4);
    call[5] = 0x83; call[6] = 0xc0; call[7] = 100;     /* add $100, %eax */
    call[8] = 0xc3;
    file_make("/tmp/rr_remap_1", ret1, sizeof ret1);
    file_make("/tmp/rr_remap_2", ret2, sizeof ret2);
    file_make("/tmp/rr_remap_caller", call, 9);
    map_at(CALLER, "/tmp/rr_remap_caller");
    if (argc > 1) {
        char path[64], label[16];
        snprintf(path, sizeof path, "/tmp/rr_remap_%d", atoi(argv[1]));
        snprintf(label, sizeof label, "file %d", atoi(argv[1]));
        map_at(CODE, path);
        calls(label);
        return 0;
    }
    map_at(CODE, "/tmp/rr_remap_1");
    calls("1");
    map_at(CODE, "/tmp/rr_remap_2");
    calls("2 over 1");
    map_at(CODE, "/tmp/rr_remap_1");
    calls("1 over 2");
    if (mprotect((void *)CODE, 4096, PROT_READ | PROT_WRITE | PROT_EXEC)) { perror("mprotect"); return 1; }
    memcpy((void *)(CODE + ENTRY), ret3, sizeof ret3);
    calls("written 3");
    memcpy((void *)(CODE + ENTRY), ret4, sizeof ret4);
    calls("written 4");
    munmap((void *)CODE, 4096);
    munmap((void *)CALLER, 4096);
    return 0;
}
