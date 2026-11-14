/* hello.c - first Intel program with dyld and libSystem */
#include <stdio.h>

int main(int argc, char **argv)
{
    int i;
    printf("Hello from the Intel program, %d arguments\n", argc);
    for (i = 0; i < argc; i++) printf("  %d: %s\n", i, argv[i]);
    return 7;
}
