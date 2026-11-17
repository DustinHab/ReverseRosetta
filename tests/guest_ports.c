/* guest_ports.c - two things Firefox 16 ran into at startup (i386 only, against tests/guest_ports.expected):
 * (1) dyld maps a whole library file read-only to look at it, unmaps it with the unrounded file size, then maps the
 *     segments into the same range; a later vm_protect with write access there made the translator copy the old,
 *     larger shared mapping into private memory and read past the end of what was mapped (crash).
 * (2) Breakpad sets the task's exception ports; a fault in the translator itself (its own look at the guest's ebp
 *     chain at munmap, with code that has no frame pointers) then went as a Mach message to a guest thread, which
 *     waited for the thread lock the faulting thread held (hang). An alarm ends the test if it hangs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <mach/mach.h>

static void hung(int s) { (void)s; printf("hung\n"); fflush(stdout); _exit(1); }

/* munmap with ebp pointing into nothing (as in code without frame pointers) */
static int munmap_bad_ebp(void *p, size_t n)
{
    int r;
    __asm__ volatile("pushl %%ebp\n\t"
                     "movl $0x00100004, %%ebp\n\t"
                     "subl $12, %%esp\n\t"
                     "movl %1, (%%esp)\n\t"
                     "movl %2, 4(%%esp)\n\t"
                     "call _munmap\n\t"
                     "addl $12, %%esp\n\t"
                     "popl %%ebp"
                     : "=a"(r) : "r"(p), "r"(n) : "ecx", "edx", "memory");
    return r;
}

int main(void)
{
    const char *path = "/tmp/rr_guest_ports.bin";
    size_t size = 0x5a84, k;
    unsigned char *buf = malloc(size), *p, *q;
    unsigned sum = 0;
    mach_port_t port;
    kern_return_t kr;
    int fd;
    signal(SIGALRM, hung);
    alarm(20);
    for (k = 0; k < size; k++) buf[k] = (unsigned char)(k * 7 + 3);
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, buf, size) != (ssize_t)size) return 1;
    p = mmap(0, size, PROT_READ, MAP_PRIVATE, fd, 0);
    printf("whole file mapped: %s, byte 0x5000 %02x\n", p != MAP_FAILED ? "yes" : "no", p[0x5000]);
    munmap(p, size);                                        /* unrounded length */
    q = mmap(p, 0x2000, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0);
    printf("part mapped at the same place: %s\n", q == p ? "yes" : "no");
    if (mprotect(q, 0x1000, PROT_READ | PROT_WRITE) == 0) {
        q[0x10] ^= 0xff;
        for (k = 0; k < 0x2000; k++) sum = sum * 31 + q[k];
        printf("written after mprotect, checksum %08x\n", sum);
    } else printf("mprotect failed\n");
    munmap(q, 0x2000);
    close(fd);
    unlink(path);

    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
    mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
    kr = task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION, port,
                                  EXCEPTION_DEFAULT, THREAD_STATE_NONE);
    printf("task_set_exception_ports: %d\n", kr);
    p = mmap(0, 0x3000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    printf("munmap with ebp into nothing: %d\n", munmap_bad_ebp(p, 0x3000));
    printf("done\n");
    return 0;
}
