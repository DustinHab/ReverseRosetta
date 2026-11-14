/* host_symbol.c ADDR... - symbol for addresses in the shared library region (at the same place in every process)
 * Build: gcc-4.2 -o host_symbol host_symbol.c -framework ApplicationServices -framework Carbon -framework Cocoa */
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>

int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        void *a = (void *)strtoul(argv[i], 0, 16);
        Dl_info d;
        if (dladdr(a, &d) && d.dli_sname)
            printf("%s = %s+0x%lx (%s)\n", argv[i], d.dli_sname, (unsigned long)((char *)a - (char *)d.dli_saddr), d.dli_fname);
        else printf("%s = ?\n", argv[i]);
    }
    return 0;
}
