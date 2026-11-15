/* calculate.c - Calculator's expression engine (private Calculate.framework): the i386 program gets the PowerPC
 * engine through rosetta's native replacement; the output must match the native build */
#include <stdio.h>

extern int CalculatePerformExpression(char *expression, int significantDigits, int flags, char *answer);

int main(void)
{
    static char *const expressions[] = { "1+2*3", "2^10", "sqrt(2)", "10/4", "(3+4)*(5-2)", "1/0", "pi", "2 +", 0 };
    int k;
    for (k = 0; expressions[k]; k++) {
        char answer[1024] = "";
        int r = CalculatePerformExpression(expressions[k], 16, 1, answer);
        printf("%-12s -> %d \"%s\"\n", expressions[k], r, answer);
    }
    return 0;
}
