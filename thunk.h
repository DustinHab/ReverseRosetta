/* thunk.h - replace functions of the Intel libSystem with native ones (math, strings) */
#ifndef THUNK_H
#define THUNK_H

#include "cpu.h"

typedef int (*Stub)(Cpu *);

/* stub for the entry address eip (or 0). Reads the symbol table of an image at its first hit. */
Stub thunk_find(uint32_t eip);
void thunk_stats(void);

/* stub behind the symbol stub (jump table entry or jmp *pointer) at z, via the symbol name (or 0) */
Stub thunk_stub(uint32_t z);

/* read the images again (after munmap/mmap of the guest) */
void thunk_forget(void);

/* guest address of an export that was replaced by fn (the first one found), 0 if none: alGetProcAddress */
uint32_t thunk_guest_addr(Stub fn);
/* address of an exported symbol (with underscore) of the loaded guest image whose install name contains part, or 0 */
uint32_t thunk_guest_symbol(const char *part, const char *sym);

/* name of the stub (for the statistics) */
const char *thunk_name(Stub fn);

/* kind of the stub: 'g' only via the C wrapper; '1' double(double), '2' double(double, double), 'f' float(float),
   'F' float(float, float) with the PowerPC function in *native (arguments in f1/f2, result in f1) */
char thunk_kind(Stub fn, void **native);

#endif
