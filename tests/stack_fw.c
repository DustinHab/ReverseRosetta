/* stack_fw.c - forwarding of stack slots inside a translated block: values stored to [ebp+d] / [esp+d] are read back
 * from registers. Loops whose block starts after the prologue (ebp unknown relative to esp at the block head), the
 * same slot written through esp and read through ebp, read-modify-write of slots (or, inc, rol, not, shl), xchg and
 * xadd with a slot, writes through a pointer, 16- and 8-bit writes into a 32-bit slot, and a call that changes its
 * argument slot. Translated (ROSETTA_THRESHOLD=1) against the interpreter (ROSETTA_INTERP=1). */
#include <stdio.h>

unsigned int fw_alias(unsigned int n, unsigned int v);
unsigned int fw_rmw(unsigned int n, unsigned int v);
unsigned int fw_xchg(unsigned int n, unsigned int v);
unsigned int fw_pointer(unsigned int n, unsigned int v);
unsigned int fw_partial(unsigned int n, unsigned int v);
unsigned int fw_call(unsigned int n, unsigned int v);

/* frame: ebp-4 ebx, esp = ebp - 32 (so [esp+24] is [ebp-8], [esp+20] is [ebp-12]) */
#define PROLOGUE "pushl %ebp\n\tmovl %esp, %ebp\n\tpushl %ebx\n\tsubl $28, %esp\n\t" \
                 "movl 8(%ebp), %ecx\n\tmovl 12(%ebp), %edx\n\txorl %eax, %eax\n"
#define EPILOGUE "\taddl $28, %esp\n\tpopl %ebx\n\tpopl %ebp\n\tret\n"

__asm__(
    ".text\n"
    /* the same slot through esp and through ebp */
    ".globl _fw_alias\n_fw_alias:\n\t" PROLOGUE
    "1:\tmovl %edx, -8(%ebp)\n\t"
    "leal 1(%edx), %ebx\n\t"
    "movl %ebx, 24(%esp)\n\t"           /* [ebp-8] = v + 1 */
    "addl -8(%ebp), %eax\n\t"           /* + (v + 1), not v */
    "movl %eax, 20(%esp)\n\t"           /* [ebp-12] through esp */
    "movl $5, -12(%ebp)\n\t"            /* ... overwritten through ebp */
    "xorl 20(%esp), %eax\n\t"           /* ^ 5 */
    "roll $3, %eax\n\t"
    "addl $7, %edx\n\t"
    "decl %ecx\n\t"
    "jnz 1b\n" EPILOGUE

    /* read-modify-write of slots while the stored registers change */
    ".globl _fw_rmw\n_fw_rmw:\n\t" PROLOGUE
    "1:\tmovl %edx, -8(%ebp)\n\t"
    "movl %eax, -12(%ebp)\n\t"
    "movl %ecx, %edx\n\t"               /* the stored edx changes */
    "xorl -8(%ebp), %eax\n\t"
    "orl %eax, -12(%ebp)\n\t"
    "movl -12(%ebp), %ebx\n\t"
    "addl %ebx, %eax\n\t"
    "incl -8(%ebp)\n\t"
    "movl -8(%ebp), %edx\n\t"
    "roll $5, -12(%ebp)\n\t"
    "subl -12(%ebp), %eax\n\t"
    "notl -8(%ebp)\n\t"
    "xorl -8(%ebp), %eax\n\t"
    "shll $3, -12(%ebp)\n\t"
    "addl -12(%ebp), %eax\n\t"
    "addl $0x9e3779b9, %edx\n\t"
    "decl %ecx\n\t"
    "jnz 1b\n" EPILOGUE

    /* xchg and xadd change the register after the store */
    ".globl _fw_xchg\n_fw_xchg:\n\t" PROLOGUE
    "1:\tmovl %eax, -8(%ebp)\n\t"
    "movl %edx, %ebx\n\t"
    "xchgl %ebx, -8(%ebp)\n\t"          /* ebx = eax, slot = edx */
    "addl -8(%ebp), %eax\n\t"
    "xaddl %eax, -8(%ebp)\n\t"          /* slot += eax, eax = old slot */
    "addl -8(%ebp), %ebx\n\t"
    "xorl %ebx, %eax\n\t"
    "roll $7, %eax\n\t"
    "addl $0x3c6ef372, %edx\n\t"
    "decl %ecx\n\t"
    "jnz 1b\n" EPILOGUE

    /* a write through a pointer into the slot */
    ".globl _fw_pointer\n_fw_pointer:\n\t" PROLOGUE
    "1:\tmovl %edx, -8(%ebp)\n\t"
    "leal -8(%ebp), %ebx\n\t"
    "movl %eax, (%ebx)\n\t"
    "addl -8(%ebp), %edx\n\t"           /* + eax, not + edx */
    "movl %edx, -12(%ebp)\n\t"
    "incl (%ebx)\n\t"
    "addl -8(%ebp), %eax\n\t"
    "xorl -12(%ebp), %eax\n\t"
    "roll $11, %eax\n\t"
    "addl $0x6b, %eax\n\t"
    "decl %ecx\n\t"
    "jnz 1b\n" EPILOGUE

    /* 16- and 8-bit writes into a remembered 32-bit slot */
    ".globl _fw_partial\n_fw_partial:\n\t" PROLOGUE
    "1:\tmovl %edx, -8(%ebp)\n\t"
    "movw %ax, -8(%ebp)\n\t"
    "addl -8(%ebp), %eax\n\t"
    "movb %cl, -5(%ebp)\n\t"
    "xorl -8(%ebp), %edx\n\t"
    "movl %eax, -12(%ebp)\n\t"
    "incb -12(%ebp)\n\t"
    "addl -12(%ebp), %edx\n\t"
    "roll $13, %edx\n\t"
    "decl %ecx\n\t"
    "jnz 1b\n\t"
    "addl %edx, %eax\n" EPILOGUE

    /* the called function changes its argument slot */
    "_fw_bump:\n\t"
    "incl 4(%esp)\n\t"
    "movl 4(%esp), %eax\n\t"
    "ret\n"
    ".globl _fw_call\n_fw_call:\n\t" PROLOGUE
    "1:\tmovl %edx, (%esp)\n\t"
    "movl %ecx, -8(%ebp)\n\t"
    "call _fw_bump\n\t"
    "addl (%esp), %eax\n\t"             /* the incremented argument */
    "movl -8(%ebp), %ecx\n\t"
    "addl $0x1234567, %edx\n\t"
    "decl %ecx\n\t"
    "jnz 1b\n" EPILOGUE);

/* the same in C: locals that live in memory */
static unsigned int c_locals(unsigned int n, unsigned int v)
{
    volatile unsigned int a = v, b = v ^ 0x55, c = 0;
    unsigned int i, *p = (unsigned int *)&b;
    for (i = 0; i < n; i++) {
        a += b;
        b ^= a >> 3;
        c |= a & 0xf0f0;
        *p += i;
        c += b;
        a = (a << 5) | (a >> 27);
    }
    return a ^ b ^ c;
}

int main(void)
{
    unsigned int v[] = { 0, 1, 0x7fffffffu, 0x80000000u, 0xdeadbeefu, 0x12345678u };
    unsigned int k;
    for (k = 0; k < sizeof v / sizeof v[0]; k++)
        printf("%08x: alias %08x rmw %08x xchg %08x pointer %08x partial %08x call %08x c %08x\n", v[k],
               fw_alias(1000, v[k]), fw_rmw(1000, v[k]), fw_xchg(1000, v[k]), fw_pointer(1000, v[k]),
               fw_partial(1000, v[k]), fw_call(1000, v[k]), c_locals(1000, v[k]));
    return 0;
}
