/* raw_layout.c - layout of the fields in system structures under i386 (runs in our own interpreter, without
   libSystem) */
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/dirent.h>
#include <sys/signal.h>
#include <sys/termios.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <mach/message.h>

static int sys3(int n, int a, int b, int c)
{
    int r;
    __asm__ volatile("pushl %4; pushl %3; pushl %2; pushl $0; int $0x80; addl $16,%%esp"
                     : "=a"(r) : "a"(n), "g"(a), "g"(b), "g"(c) : "memory");
    return r;
}

static void line(const char *name, unsigned v)
{
    char t[96];
    int i = 0, k;
    while (name[i]) { t[i] = name[i]; i++; }
    t[i++] = ' ';
    for (k = 3; k >= 0; k--) t[i++] = "0123456789abcdef"[(v >> (k * 4)) & 15];
    t[i++] = '\n';
    sys3(4, 1, (int)t, i);
}

#define O(s, f) line(#s "." #f, offsetof(struct s, f))
#define G(s) line("sizeof " #s, sizeof(struct s))

void start(void)
{
    G(stat); O(stat, st_dev); O(stat, st_ino); O(stat, st_mode); O(stat, st_nlink); O(stat, st_uid); O(stat, st_gid);
    O(stat, st_rdev); O(stat, st_atimespec); O(stat, st_mtimespec); O(stat, st_ctimespec); O(stat, st_size);
    O(stat, st_blocks); O(stat, st_blksize); O(stat, st_flags); O(stat, st_gen);
    G(stat64); O(stat64, st_dev); O(stat64, st_mode); O(stat64, st_nlink); O(stat64, st_ino); O(stat64, st_uid);
    O(stat64, st_gid); O(stat64, st_rdev); O(stat64, st_atimespec); O(stat64, st_mtimespec); O(stat64, st_ctimespec);
    O(stat64, st_birthtimespec); O(stat64, st_size); O(stat64, st_blocks); O(stat64, st_blksize); O(stat64, st_flags);
    O(stat64, st_gen);
    G(statfs); O(statfs, f_bsize); O(statfs, f_iosize); O(statfs, f_blocks); O(statfs, f_bfree); O(statfs, f_bavail);
    O(statfs, f_files); O(statfs, f_ffree); O(statfs, f_fsid); O(statfs, f_owner); O(statfs, f_type); O(statfs, f_flags);
    O(statfs, f_fstypename); O(statfs, f_mntonname); O(statfs, f_mntfromname);
    G(statfs64); O(statfs64, f_bsize); O(statfs64, f_iosize); O(statfs64, f_blocks); O(statfs64, f_files);
    O(statfs64, f_fsid); O(statfs64, f_owner); O(statfs64, f_type); O(statfs64, f_flags); O(statfs64, f_fssubtype);
    O(statfs64, f_fstypename); O(statfs64, f_mntonname); O(statfs64, f_mntfromname);
    G(dirent); O(dirent, d_ino); O(dirent, d_reclen); O(dirent, d_type); O(dirent, d_namlen); O(dirent, d_name);
    G(rlimit); O(rlimit, rlim_max);
    G(rusage);
    G(timeval); G(timespec);
    G(__sigaction); O(__sigaction, __sigaction_u); O(__sigaction, sa_tramp); O(__sigaction, sa_mask); O(__sigaction, sa_flags);
    G(termios); O(termios, c_cc); O(termios, c_ispeed); O(termios, c_ospeed);
    G(winsize);
    G(sockaddr);
    sys3(1, 0, 0, 0);
    for (;;) ;
}
