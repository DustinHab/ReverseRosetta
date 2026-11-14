/* mach_msg.h - Mach traps and messages of the guest */
#ifndef MACH_MSG_H
#define MACH_MSG_H

#include "cpu.h"

/* Mach trap with number n (positive), arguments from basis+4 */
void mach_trap(Cpu *c, int n, uint32_t base);

extern int g_log;

#endif
