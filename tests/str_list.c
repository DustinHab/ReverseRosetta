/* str_list.c - strlen/strchr/memchr/strcmp at all positions, one line per case (for comparison with diff) */
#include <stdio.h>
#include <string.h>

int main(void)
{
    static char text[300];
    int i, j;
    for (i = 0; i < 64; i++)
        for (j = 1; j < 70; j++) {
            memset(text, 'a' + (i + j) % 26, sizeof text);
            text[i + j] = 0;
            printf("%d %d len %d", i, j, (int)strlen(text + i));
            text[i + j / 2] = 'Z';
            printf(" chr %d", strchr(text + i, 'Z') ? (int)(strchr(text + i, 'Z') - text) : -1);
            printf(" mem %d", memchr(text + i, 'Z', (size_t)j) ? 1 : 0);
            printf(" cmp %d\n", strcmp(text + i, text + 1) > 0);
        }
    return 0;
}
