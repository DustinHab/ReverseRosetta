/* commpage.c - Intel commpage at 0xffff0000.
 *
 * The i386 libSystem jumps to fixed addresses from 0xffff0000 for atomics, locks, memcpy etc. and reads the
 * capabilities there (+0x20). The layout comes from the callers in the i386 libSystem (tools/commpage_users.sh):
 *
 * Caution: libSystem calls +0x80, +0xc0, +0x1a0, +0x5e0, +0x5f0 with "call *address" (FF 15): there is a pointer
 * to the code there, not the code itself. It jumps to the others directly ("movl $address,%eax; jmp *%eax").
 *
 *   +0x020 capabilities (uint32)       +0x080 CAS32 (eax old, edx new, ecx pointer; ZF)
 *   +0x0c0 CAS64 (edx:eax old, ecx:ebx new, esi pointer; ZF)            +0x180 memory barrier
 *   +0x1a0 atomic add (xadd eax,(edx))   +0x220 spin_lock_try   +0x260 spin_lock   +0x2a0 spin_unlock
 *   +0x2e0 gettimeofday (returns 1 = "use the system call")         +0x4e0/+0x520 flush cache
 *   +0x5e0 bts eax,(edx)   +0x5f0 btr eax,(edx)   +0x600 bzero   +0x780 bcopy   +0x7a0 memcpy/memmove
 *   +0x1000 memset_pattern   +0x1700 mach_absolute_time
 *
 * Small routines are here as real Intel code. bzero, bcopy, memcpy, memmove and mach_absolute_time run
 * natively (commpage_native), called when eip points to their start.
 */
#include "cpu.h"
#include "commpage.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_time.h>

#define CAP_MMX 0x1
#define CAP_SSE 0x2
#define CAP_SSE2 0x4
#define CAP_SSE3 0x8
#define CAP_CACHE64 0x20
#define CAP_UP 0x8000

static void code(uint32_t a, const uint8_t *c, size_t n) { memcpy((void *)(uintptr_t)a, c, n); }

/* entries that libSystem calls via "call *address": there is a pointer to the code there (here from +0x1800) */
static void ptr_code(uint32_t ent, uint32_t target, const uint8_t *c, size_t n)
{
    st32(COMMPAGE_BASE + ent, COMMPAGE_BASE + target);
    code(COMMPAGE_BASE + target, c, n);
}

int commpage_create(void)
{
    vm_address_t a = COMMPAGE_BASE;
    if (vm_allocate(mach_task_self(), &a, COMMPAGE_SIZE, FALSE) != KERN_SUCCESS) {
        fprintf(stderr, "rosetta: space for the Intel commpage (0xffff0000) is occupied\n");
        return -1;
    }
    memcpy((void *)(uintptr_t)COMMPAGE_BASE, "commimpa", 8);
    st16(COMMPAGE_BASE + 0x1e, 7);
    st32(COMMPAGE_BASE + 0x20, CAP_MMX | CAP_SSE | CAP_SSE2 | CAP_SSE3 | CAP_CACHE64 | CAP_UP | (1u << 16));
    st16(COMMPAGE_BASE + 0x26, 64);
    {   /* lock cmpxchg %edx,(%ecx); ret */
        static const uint8_t c[] = { 0xf0, 0x0f, 0xb1, 0x11, 0xc3 };
        ptr_code(0x80, 0x1800, c, sizeof c);
    }
    {   /* lock cmpxchg8b (%esi); ret */
        static const uint8_t c[] = { 0xf0, 0x0f, 0xc7, 0x0e, 0xc3 };
        ptr_code(0xc0, 0x1810, c, sizeof c);
    }
    {   /* ret (memory barrier: the interpreter is strictly ordered) */
        static const uint8_t c[] = { 0xc3 };
        code(COMMPAGE_BASE + 0x180, c, sizeof c);
        code(COMMPAGE_BASE + 0x4e0, c, sizeof c);
        code(COMMPAGE_BASE + 0x520, c, sizeof c);
    }
    {   /* lock xaddl %eax,(%edx); ret */
        static const uint8_t c[] = { 0xf0, 0x0f, 0xc1, 0x02, 0xc3 };
        ptr_code(0x1a0, 0x1820, c, sizeof c);
    }
    {   /* spin_lock_try: movl 4(%esp),%edx; xorl %eax,%eax; movl $1,%ecx; lock cmpxchgl %ecx,(%edx);
           sete %al; movzbl %al,%eax; ret */
        static const uint8_t c[] = { 0x8b, 0x54, 0x24, 0x04, 0x31, 0xc0, 0xb9, 0x01, 0x00, 0x00, 0x00,
                                     0xf0, 0x0f, 0xb1, 0x0a, 0x0f, 0x94, 0xc0, 0x0f, 0xb6, 0xc0, 0xc3 };
        code(COMMPAGE_BASE + 0x220, c, sizeof c);
    }
    {   /* spin_lock: movl 4(%esp),%edx; 1: xorl %eax,%eax; movl $1,%ecx; lock cmpxchgl %ecx,(%edx); jne 2f; ret;
           2: pause; jmp 1b */
        static const uint8_t c[] = { 0x8b, 0x54, 0x24, 0x04, 0x31, 0xc0, 0xb9, 0x01, 0x00, 0x00, 0x00,
                                     0xf0, 0x0f, 0xb1, 0x0a, 0x75, 0x01, 0xc3, 0xf3, 0x90, 0xeb, 0xee };
        code(COMMPAGE_BASE + 0x260, c, sizeof c);
    }
    {   /* spin_unlock: movl 4(%esp),%edx; movl $0,(%edx); ret */
        static const uint8_t c[] = { 0x8b, 0x54, 0x24, 0x04, 0xc7, 0x02, 0x00, 0x00, 0x00, 0x00, 0xc3 };
        code(COMMPAGE_BASE + 0x2a0, c, sizeof c);
    }
    {   /* gettimeofday: movl $1,%eax; ret -> libSystem uses the system call */
        static const uint8_t c[] = { 0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3 };
        code(COMMPAGE_BASE + 0x2e0, c, sizeof c);
    }
    {   /* lock btsl %eax,(%edx); ret  or  lock btrl %eax,(%edx); ret */
        static const uint8_t s[] = { 0xf0, 0x0f, 0xab, 0x02, 0xc3 }, r[] = { 0xf0, 0x0f, 0xb3, 0x02, 0xc3 };
        ptr_code(0x5e0, 0x1830, s, sizeof s);
        ptr_code(0x5f0, 0x1840, r, sizeof r);
    }
    return 0;
}

/* return like "ret" */
static void back(Cpu *c)
{
    c->eip = ld32(c->r[ESP]);
    c->r[ESP] += 4;
}

static inline uint32_t stackarg(Cpu *c, int i) { return ld32(c->r[ESP] + 4 + 4 * (uint32_t)i); }

int commpage_native(Cpu *c)
{
    switch (c->eip - COMMPAGE_BASE) {
    case 0x600:                             /* bzero(p, n) */
        memset((void *)(uintptr_t)stackarg(c, 0), 0, stackarg(c, 1));
        back(c);
        return 1;
    case 0x780:                             /* bcopy(src, dst, n) */
        memmove((void *)(uintptr_t)stackarg(c, 1), (const void *)(uintptr_t)stackarg(c, 0), stackarg(c, 2));
        back(c);
        return 1;
    case 0x7a0:                             /* memcpy/memmove(dst, src, n) -> dst */
        memmove((void *)(uintptr_t)stackarg(c, 0), (const void *)(uintptr_t)stackarg(c, 1), stackarg(c, 2));
        c->r[EAX] = stackarg(c, 0);
        back(c);
        return 1;
    case 0x1000: {                          /* memset_pattern: edi = destination, edx = length, xmm0 = pattern starting at edi */
        uint8_t *z = (uint8_t *)(uintptr_t)c->r[EDI];
        const uint8_t *m = c->xmm[0].b;
        uint32_t n = c->r[EDX], k;
        for (k = 1; k < 16 && m[k] == m[0]; k++) ;
        if (k == 16) memset(z, m[0], n);   /* memset with one byte: the pattern is uniform */
        else {
            for (k = 0; k + 16 <= n; k += 16) memcpy(z + k, m, 16);
            memcpy(z + k, m, n - k);
        }
        c->r[EDI] += n;
        c->r[EDX] = 0;
        back(c);
        return 1;
    }
    case 0x1700: {                          /* mach_absolute_time -> edx:eax */
        uint64_t t = mach_absolute_time();
        c->r[EAX] = (uint32_t)t;
        c->r[EDX] = (uint32_t)(t >> 32);
        back(c);
        return 1;
    }
    }
    return 0;
}
