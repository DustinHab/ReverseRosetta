/* commpage.h - Intel commpage */
#ifndef COMMPAGE_H
#define COMMPAGE_H

#include "cpu.h"

#define COMMPAGE_BASE 0xffff0000u
#define COMMPAGE_SIZE 0x2000u

int commpage_create(void);

/* eip is in the commpage: execute the native routine (1) or let the Intel code there run (0) */
int commpage_native(Cpu *c);

#endif
