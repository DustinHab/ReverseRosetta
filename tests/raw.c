/* raw.c - smallest test program without libSystem: write + exit via int $0x80 */
static int sys3(int n, int a, int b, int c)
{
    int r;
    __asm__ volatile("pushl %4; pushl %3; pushl %2; pushl $0; int $0x80; addl $16,%%esp"
                     : "=a"(r) : "a"(n), "g"(a), "g"(b), "g"(c) : "memory");
    return r;
}

void start(void)
{
    static const char t[] = "Hello from the Intel program\n";
    sys3(4, 1, (int)t, sizeof t - 1);
    sys3(1, 3, 0, 0);
    for (;;) ;
}
