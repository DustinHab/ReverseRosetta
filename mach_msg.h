/* mach_msg.h - Mach traps and messages of the guest */
#ifndef MACH_MSG_H
#define MACH_MSG_H

#include "cpu.h"

/* Mach trap with number n (positive), arguments from basis+4 */
void mach_trap(Cpu *c, int n, uint32_t base);

/* ROSETTA_STATS: Mach messages by id (count, time in mach_msg, time converting) */
void mach_msg_stats(void);

extern int g_log;

#endif
