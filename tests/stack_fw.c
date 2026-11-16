/* stack_fw.c - forwarding of stack slots inside a translated block: values stored to [ebp+d] / [esp+d] are read back
 * from registers. Loops whose block starts after the prologue (ebp unknown relative to esp at the block head), the
 * same slot written through esp and read through ebp, read-modify-write of slots (or, inc, rol, not, shl), xchg and
 * xadd with a slot, writes through a pointer, 16- and 8-bit writes into a 32-bit slot, and a call that changes its
 * argument slot. Chains of read-modify-writes computed in the slot's copy register (with carry, compare, shift by cl,
 * shld across inner jumps), and a pointer reloaded from the frame for every byte while the pointer slot changes
 * through a pointer. Stores that a later instruction overwrites (left out) while the copy registers run out. 8- and
 * 16-bit reads inside remembered slots.
 * Translated (ROSETTA_THRESHOLD=1) against the interpreter (ROSETTA_INTERP=1). */
#include <stdio.h>

unsigned int fw_alias(unsigned int n, unsigned int v);
unsigned int fw_rmw(unsigned int n, unsigned int v);
unsigned int fw_xchg(unsigned int n, unsigned int v);
unsigned int fw_pointer(unsigned int n, unsigned int v);
unsigned int fw_partial(unsigned int n, unsigned int v);
unsigned int fw_call(unsigned int n, unsigned int v);
unsigned int fw_inplace(unsigned int n, unsigned int v);
unsigned int fw_reload(unsigned int n, unsigned int v);
unsigned int fw_dead(unsigned int n, unsigned int v);
unsigned int fw_narrow(unsigned int n, unsigned int v);

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
    "jnz 1b\n" EPILOGUE

    /* read-modify-write chains on the same slots (sha1 as gcc compiles it: xor, xor, ror into one slot), flags of an
       in-place add and compare, not/neg/shift by cl/shld in place; the loop counter itself is a slot */
    ".globl _fw_inplace\n_fw_inplace:\n\t" PROLOGUE
    "movl %edx, -16(%ebp)\n\t"
    "movl %ecx, -20(%ebp)\n"
    "1:\tmovl %eax, -8(%ebp)\n\t"
    "xorl %edx, -8(%ebp)\n\t"
    "xorl -16(%ebp), %eax\n\t"
    "xorl %eax, -8(%ebp)\n\t"
    "rorl $31, -8(%ebp)\n\t"
    "movl %edx, -12(%ebp)\n\t"
    "addl %eax, -12(%ebp)\n\t"
    "jnc 2f\n\t"
    "incl %edx\n"
    "2:\tmovl -8(%ebp), %ebx\n\t"
    "subl %ebx, -12(%ebp)\n\t"
    "negl -12(%ebp)\n\t"
    "movl %ebx, -24(%ebp)\n\t"
    "shll %cl, -24(%ebp)\n\t"
    "shldl $3, %edx, -24(%ebp)\n\t"
    "cmpl -24(%ebp), %edx\n\t"
    "jb 3f\n\t"
    "addl -12(%ebp), %edx\n"
    "3:\tnotl -16(%ebp)\n\t"
    "addl -24(%ebp), %eax\n\t"
    "addl -16(%ebp), %eax\n\t"
    "roll $7, %eax\n\t"
    "decl -20(%ebp)\n\t"
    "jnz 1b\n\t"
    "addl -16(%ebp), %eax\n\t"
    "xorl %edx, %eax\n" EPILOGUE

    /* a pointer reloaded from its slot for every byte (its register changes right after the load); in between the
       pointer slot is changed through a pointer, so a remembered copy would read the wrong buffer */
    ".globl _fw_reload\n_fw_reload:\n\t" PROLOGUE
    "movl %edx, -24(%ebp)\n\t"
    "notl %edx\n\t"
    "movl %edx, -20(%ebp)\n\t"
    "leal -24(%ebp), %ebx\n\t"
    "movl %ebx, -12(%ebp)\n"
    "1:\tmovl -12(%ebp), %eax\n\t"
    "movzbl (%eax), %eax\n\t"
    "movl %eax, -8(%ebp)\n\t"
    "movl -12(%ebp), %edx\n\t"
    "movzbl 1(%edx), %edx\n\t"
    "shll $8, %edx\n\t"
    "orl %edx, -8(%ebp)\n\t"
    "leal -12(%ebp), %edx\n\t"
    "xorl $4, (%edx)\n\t"
    "movl -12(%ebp), %eax\n\t"
    "movzbl 2(%eax), %eax\n\t"
    "shll $16, %eax\n\t"
    "orl %eax, -8(%ebp)\n\t"
    "movl -12(%ebp), %eax\n\t"
    "movzbl 3(%eax), %eax\n\t"
    "roll $24, %eax\n\t"
    "orl -8(%ebp), %eax\n\t"
    "addl %eax, -24(%ebp)\n\t"
    "xorl %ecx, -20(%ebp)\n\t"
    "decl %ecx\n\t"
    "jnz 1b\n\t"
    "movl -24(%ebp), %eax\n\t"
    "xorl -20(%ebp), %eax\n" EPILOGUE

    /* stores that a later instruction overwrites (ROSETTA_DEAD=1 leaves them out): five other slots in between take the
       copy registers (the left-out store has to be written after all), the stored register changes, reads in
       between, then the slot is overwritten by a read-modify-write and by a plain mov */
    ".globl _fw_dead\n_fw_dead:\n\t" PROLOGUE
    "1:\tmovl %eax, -8(%ebp)\n\t"
    "movl %edx, -12(%ebp)\n\t"
    "addl $3, -12(%ebp)\n\t"
    "movl %ecx, -16(%ebp)\n\t"
    "xorl %eax, -16(%ebp)\n\t"
    "movl %edx, -20(%ebp)\n\t"
    "subl %ecx, -20(%ebp)\n\t"
    "movl %eax, -24(%ebp)\n\t"
    "roll $5, -24(%ebp)\n\t"
    "movl %ecx, -28(%ebp)\n\t"
    "addl %edx, -28(%ebp)\n\t"
    "movl -8(%ebp), %ebx\n\t"
    "incl %eax\n\t"
    "addl -12(%ebp), %ebx\n\t"
    "xorl -16(%ebp), %ebx\n\t"
    "addl -20(%ebp), %ebx\n\t"
    "xorl -24(%ebp), %ebx\n\t"
    "addl -28(%ebp), %ebx\n\t"
    "addl %ebx, -8(%ebp)\n\t"
    "movl %ebx, -12(%ebp)\n\t"
    "xorl %eax, -12(%ebp)\n\t"
    "rorl $7, -12(%ebp)\n\t"
    "movl -8(%ebp), %edx\n\t"
    "addl -12(%ebp), %edx\n\t"
    "movl %edx, -16(%ebp)\n\t"
    "movl %eax, -16(%ebp)\n\t"
    "addl -16(%ebp), %edx\n\t"
    "decl %ecx\n\t"
    "jnz 1b\n\t"
    "addl -8(%ebp), %eax\n\t"
    "xorl -12(%ebp), %eax\n\t"
    "addl -16(%ebp), %eax\n\t"
    "xorl -24(%ebp), %eax\n\t"
    "addl %edx, %eax\n" EPILOGUE

    /* 8- and 16-bit reads inside remembered 32-bit slots (AES: shrl $0x10,-0x24(%ebp); movzbl -0x24(%ebp),%eax): every
       byte and half, zero- and sign-extended, from a guest register, a copy register and a constant; a byte write in
       between makes the slot unknown */
    ".globl _fw_narrow\n_fw_narrow:\n\t" PROLOGUE
    "1:\tmovl %edx, -8(%ebp)\n\t"
    "movzbl -8(%ebp), %ebx\n\t"
    "addl %ebx, %eax\n\t"
    "movzbl -7(%ebp), %ebx\n\t"
    "xorl %ebx, %eax\n\t"
    "movsbl -6(%ebp), %ebx\n\t"
    "addl %ebx, %eax\n\t"
    "movsbl -5(%ebp), %ebx\n\t"
    "xorl %ebx, %eax\n\t"
    "shrl $16, -8(%ebp)\n\t"
    "movzbl -8(%ebp), %ebx\n\t"
    "addl %ebx, %eax\n\t"
    "movzwl -8(%ebp), %ebx\n\t"
    "xorl %ebx, %eax\n\t"
    "movswl -6(%ebp), %ebx\n\t"
    "addl %ebx, %eax\n\t"
    "movl %eax, -12(%ebp)\n\t"
    "roll $9, -12(%ebp)\n\t"
    "movb -10(%ebp), %bl\n\t"
    "movb -11(%ebp), %bh\n\t"
    "addl %ebx, %eax\n\t"
    "movzwl -10(%ebp), %ebx\n\t"
    "xorl %ebx, %eax\n\t"
    "movl $0x89abcdef, -16(%ebp)\n\t"
    "movzbl -15(%ebp), %ebx\n\t"
    "addl %ebx, %eax\n\t"
    "movswl -14(%ebp), %ebx\n\t"
    "xorl %ebx, %eax\n\t"
    "movb %al, -15(%ebp)\n\t"
    "movzwl -16(%ebp), %ebx\n\t"
    "addl %ebx, %eax\n\t"
    "cmpb -9(%ebp), %al\n\t"
    "adcl $0, %eax\n\t"
    "addb -12(%ebp), %al\n\t"
    "roll $5, %eax\n\t"
    "addl $0x9e3779b9, %edx\n\t"
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
        printf("%08x: alias %08x rmw %08x xchg %08x pointer %08x partial %08x call %08x c %08x inplace %08x "
               "reload %08x dead %08x narrow %08x\n", v[k],
               fw_alias(1000, v[k]), fw_rmw(1000, v[k]), fw_xchg(1000, v[k]), fw_pointer(1000, v[k]),
               fw_partial(1000, v[k]), fw_call(1000, v[k]), c_locals(1000, v[k]), fw_inplace(1000, v[k]),
               fw_reload(1000, v[k]), fw_dead(1000, v[k]), fw_narrow(1000, v[k]));
    return 0;
}
