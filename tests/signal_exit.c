/* signal_exit.c - SIGTERM without a handler terminates the process (exit status 128 + 15 to the shell), nothing
   after that */
#include <stdio.h>
#include <signal.h>

int main(void)
{
    printf("before\n");
    fflush(stdout);
    raise(SIGTERM);
    printf("after\n");
    return 0;
}
