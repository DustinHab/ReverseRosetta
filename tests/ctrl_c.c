/* ctrl_c.c - SIGINT from outside to a program in an endless compute loop with its own handler */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

static volatile unsigned g_n;

static void catcher(int s)
{
    (void)s;
    printf("SIGINT received\n");
    fflush(stdout);
    _exit(3);
}

int main(void)
{
    signal(SIGINT, catcher);
    printf("computing\n");
    fflush(stdout);
    for (;;) g_n = g_n * 3 + 1;
    return 0;
}
