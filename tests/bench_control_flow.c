/* bench_control_flow.c - benchmark load with many calls: recursion, qsort with a comparison function, switch table (indirect
   jump), strings. Usage: bench_control_flow [rounds] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static int cmp(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : x > y;
}

/* small stack machine: switch over opcodes */
static int machine(const unsigned char *code, int n, int x)
{
    int stack[16], sp = 0, i;
    for (i = 0; i < n; i++) {
        switch (code[i]) {
        case 0: stack[sp++] = x; break;
        case 1: stack[sp - 2] += stack[sp - 1]; sp--; break;
        case 2: stack[sp - 2] *= stack[sp - 1]; sp--; break;
        case 3: stack[sp - 1] ^= 0x5a5a; break;
        case 4: stack[sp++] = i; break;
        case 5: stack[sp - 1] >>= 1; break;
        case 6: stack[sp - 2] -= stack[sp - 1]; sp--; break;
        case 7: x = stack[--sp]; break;
        }
    }
    return x;
}

static unsigned words(const char *t)
{
    char copy[256], *w;
    unsigned h = 0;
    strcpy(copy, t);
    for (w = strtok(copy, " "); w; w = strtok(0, " ")) h = h * 33 + (unsigned)strlen(w);
    return h;
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 1, r, i;
    static int field[20000];
    static const unsigned char prog[] = { 0, 4, 1, 0, 2, 3, 0, 4, 6, 5, 1, 7, 0, 0, 1, 4, 2, 5, 7 };
    unsigned h = 0, x = 7;
    for (r = 0; r < rounds; r++) {
        h += (unsigned)fib(24);
        for (i = 0; i < 20000; i++) { x = x * 1103515245u + 12345u; field[i] = (int)(x >> 4); }
        qsort(field, 20000, sizeof field[0], cmp);
        h += (unsigned)field[777];
        for (i = 0; i < 20000; i++) h += (unsigned)machine(prog, sizeof prog, i);
        for (i = 0; i < 2000; i++) h ^= words("the speeding russet vixen springs above the sleepy mutt");
    }
    printf("result %08x\n", h);
    return 0;
}
