/* jit.c - translator i386 -> PowerPC (G4).
 *
 * Registers: Intel registers fixed in r14..r21 (eax ecx edx ebx esp ebp esi edi), r22 = Cpu*, r23 = jump table,
 * r24..r27 = state of the last flag-setting operation (result, operand a, operand b, carry before), in the same
 * format as the interpreter (cc_op/cc_res/cc_a/cc_b/cc_cin): at every boundary either one can take over.
 * Temporaries: r0, r3..r12 (volatile).
 *
 * Blocks are superblocks: conditional jumps are side exits, the block continues on the fall-through path;
 * unconditional forward jumps are followed. A block ends at call, ret, indirect jumps, backward jumps into the
 * block or after MAX_INSNS.
 *
 * Flags: per block, computed backwards: which flags are still read after each instruction (looking into the
 * following blocks and at the calling convention: flags are dead at call and ret). Only flags that are read are
 * recorded. If the flag-setting instruction is directly followed by its only reader (jcc, setcc, cmovcc), both are
 * merged into one compare in CR0 (cmpw, cmplw, and., addic. ...).
 *
 * Memory: guest address = host address, lwbrx/stwbrx/lhbrx/sthbrx/lbzx/stbx with a register pair.
 *
 * Fixed targets are chained directly after the first run (the exit stub becomes "b target", the conditional jump
 * before it points straight at the target where possible), indirect targets (ret, jmp/call *) go through a table in
 * the generated code. What the translator cannot do, the interpreter executes (exit "interpret").
 *
 * Blocks from writable pages (dyld rewrites its jump stubs in __IMPORT) check their bytes on entry;
 * mmap/munmap/mprotect of the guest discard the blocks that rely on the range at the next stop (invalidate_ranges).
 * A memory fault in guest code goes to the guest's handler (jit_fault_state); the translator's own reads of guest
 * memory at translation time are probes that a fault jumps back from (safe_read).
 *
 * ROSETTA_PROFILE=1: samples of the program counter, report at the end (hottest blocks, /tmp/rosetta_profile.s).
 */
#include "jit.h"
#include "interp.h"
#include "decoder.h"
#include "commpage.h"
#include "thunk.h"
#include "sys.h"
#include "thread.h"
#include "mach_msg.h"
#include "ppc.h"
#include "macho.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/ucontext.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <libkern/OSCacheControl.h>
#include <dlfcn.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sched.h>
#include <setjmp.h>
#include <pthread.h>
#include <sys/resource.h>

#define R(i) (14 + (i))
#define RCPU 22
#define RTAB 23
#define RE 24
#define RA 25
#define RB 26
#define RCIN 27
#define RSHADOW 13                    /* pointer into the return stack */
#define OFF(f) ((int)offsetof(Cpu, f))

#define CACHE_ADDR 0x30000000u       /* fixed and free (0x10400000 was often already taken): the translation cache
                                           needs the same address in every run; guest libraries start at 0x40000000 */
/* Code region: two halves of 32 MB, the reach of b (24 MB in one piece until 17 Nov: Firefox's startup alone is 30 MB
   of code). Code fills the first half (the one the translation cache saves), then the second. Each half reaches its
   own exits (the prologue at the start of the first, copies at the start of the second) and ends in a pool for jumps
   into the other half; a chained exit stub jumps there in place with lis/ori/mtctr/bctr (r0 and ctr are free at every
   block boundary) */
#define CACHE_SIZE (64u << 20)
#define HALF_WORDS ((32u << 20) / 4)
#define POOL_WORDS (64u * 1024)             /* 256 KB per half: block starts redirected into the other half */
#define TABLE 65536                   /* entries of the jump table (eip, code) */
#define MAX_INSNS 128                 /* array size; the limit per block is g_max_insns */
static int g_max_insns = 128;         /* ROSETTA_MAX_INSNS: Intel instructions per superblock (48 until 16.11.:
                                         128 gives bzip2 -9 %, SHA-1 -7 %, startup unchanged) */
#define FRAME_SIZE 400                       /* 64..167 scratch area, 168..311 f14..f31, 320 VRSAVE, 324.. r13..r31 */
#define FSAFE 168

/* flags (own mask) */
#define FCF 1
#define FPF 2
#define FAF 4
#define FZF 8
#define FSF 16
#define FOF 32
#define TRAP 63

/* exit kinds */
#define KIND_NORMAL 0
#define KIND_INTERPRET 1
#define KIND_INVALID 2
#define KIND_WATCHDOG 4                  /* predicted register value is wrong: retranslate without prediction */
#define KIND_RET 8                     /* check at the loop entry failed: retranslate without deferred write-back */
#define KIND_INTERRUPT 16             /* halted block start (jit_interrupt): eip = host address of the start */

static uint32_t *g_cache, *g_cache_start, *g_free, *g_cache_end;
static uint32_t *g_table;
static uint16_t g_tab_slot[TABLE];   /* used slots of the jump table (flush) */
static uint32_t g_tab_n;
static int g_tab_full;
static struct { uint32_t eip; uint32_t *code; } *g_map;
static uint32_t *g_inuse, g_nused;  /* used slots of the map: a flush costs only as much as is used */
/* start small (grow at half occupancy): 2^14 entries touched 32 pages at startup for ~250 blocks */
static uint32_t g_map_bits = 10, g_map_mask = (1u << 10) - 1;
#define MAP_H(eip) (((eip) * 2654435761u) >> (32 - g_map_bits))
static uint32_t g_shadow_base;       /* return stack (ring buffer, aligned to its size) */
static void (*g_entry)(Cpu *, uint32_t *);
static uint32_t *g_exit, *g_link, *g_interrupt;
static uint32_t *g_exit2, *g_link2, *g_interrupt2;    /* the same at the start of the second half */
static uint32_t *g_half2, *g_half2_code;               /* second half; its code after the copies */
static uint32_t *g_half_end[2], *g_pool[2];            /* end of each half's code (its pool follows), pool in use */
static unsigned long g_st_far;                         /* jumps into the other half */
static int g_flush, g_careful, g_no_fusion, g_no_slots, g_ranges_old, g_poison;
static int g_no_narrow;                /* ROSETTA_NO_NARROW: 8/16-bit or/and/xor with a constant through extract/insert */
static unsigned long g_st_blocks, g_st_insns, g_st_interp, g_st_linked, g_st_flushes, g_st_invalid;
static unsigned long g_st_fusion, g_st_direct, g_st_pages, g_st_inline, g_st_table_miss, g_st_ic, g_st_fp_direct;
static int step_counted(Cpu *c);
static void interp_tally(uint32_t eip);
static void xf_all_forget(void);
static void xf_all_back(void);
static void xf_forget(int n);
static int fp_can(const Insn *b);
static int pd_can(const Insn *b);
static int pd_slot_op(const Insn *b);
static void pd_swap(const Insn *b, int z);
static void gen_pd(const Insn *b);
static int is_fp_cmp(const Insn *b);
static void m_load(int f, const Insn *b, char kind);
static int m_pre(const Insn *b, char kind);
static void m_save(int f, const Insn *b, char kind);
static void tmp_save(int z, char kind, int a);
static int tmp_new(void);
static int ea_in(const Insn *b);
static void x7_all_back(void);
static int x87_can_full(const Insn *b);
static inline int x87_can(const Insn *b) { return (unsigned)(b->op - 0xd8) <= 7u && x87_can_full(b); }
static inline int is_slot(const Insn *b, int base);
static int is_x87_cmp(const Insn *b);
static int writes_reg(const Insn *b, int r);
static uint8_t write_mask(const Insn *b);
static int mem_access(const Insn *b, int *sz, int *reads, int *writes);
static int is_push(const Insn *b);
static int (*g_step_fn)(Cpu *) = step;
static int g_vt_on;                   /* ROSETTA_STATS: translation time; =2 also its page faults (task_info twice per
                                         block, ~50 us each: it made the second start of Calculator look 2.2 times slower) */
static int g_align = 32;           /* align loop heads (ROSETTA_ALIGN=0/16/32) */
static Cpu *g_cpu;                      /* guest state at translation (= at the first entry into the block) */
static Cpu *g_fcpu[256];                /* Cpus of all threads (a flush resets their return rings) */
static int g_nfcpu;
static uint32_t g_kaddr;                 /* memory operand of the current instruction is a constant at this address */
static int g_no_const;                /* ROSETTA_NO_CONST: do not predict constants */
static int g_const_calls = -1;        /* ROSETTA_CONST_CALLS=0: no constants in blocks with calls */
static int pool_load(int f, char kind);
static void cache_load(void);
static void cpool_flush(void);
static int g_vl_cur;                    /* memory operand of the current instruction comes from the prefetch ring (stream + 1) */
static void vl_load(int f, const Insn *b);
static void vl_flush(void);
static int g_no_prefetch;              /* ROSETTA_NO_PREFETCH: do not hoist array accesses */
static void fw_flush(void);
static void fw_c_call(void);
static void fw_shadow_write(int q, int sz);
static int g_no_fw;                   /* ROSETTA_NO_FW: no store forwarding */
static int g_no_pd;                   /* ROSETTA_NO_PD: packed doubles through the direct call (C) */
static int g_no_xfh;                  /* ROSETTA_NO_XFH: high halves not in f24..f31 */
static int g_no_native;                /* ROSETTA_NO_NATIVE: math stubs through the C wrapper */
static int g_no_embed;            /* ROSETTA_NO_EMBED: do not inline direct calls */
static int g_no_sw;                   /* ROSETTA_NO_SW: no forwarding through the Intel stack */
static int g_no_sw2;                  /* ROSETTA_NO_SW2: only mov, only esp-known slots (forwarding as before 16.11.) */
static int g_no_inplace;              /* ROSETTA_NO_INPLACE: stack slot copies only through r3/r4 (as before 17.11.) */
static int g_no_loadcopy;             /* ROSETTA_NO_LOADCOPY: a loaded slot is forgotten when its register changes */
static int g_loadcopy_dist = 16;      /* ROSETTA_LOADCOPY_DIST: ... unless it is read again within this many instructions
                                         (aes over 40 MB: 4-8 2.96 s, 12 2.90, 16 2.85, 24 3.00, unlimited 3.13;
                                         sha1 the same from 4 on) */
static int g_no_narrowfw;             /* ROSETTA_NO_NARROWFW: 8/16-bit reads of a remembered slot go through memory */
static int g_no_taken;                /* ROSETTA_NO_TAKEN: blocks follow only the fall-through of conditional jumps */
static unsigned long g_st_taken;
static int g_no_far;                  /* ROSETTA_NO_FAR: compare and reader merged only when adjacent */
static int g_no_mmx;                  /* ROSETTA_NO_MMX: MMX in the interpreter (as before 17.11.) */
/* integer move, load or store whose code touches no CR field (between a merged compare and its reader) */
static int cr0_safe(const Insn *y)
{
    int op = y->op;
    if (y->rep || y->lock || (y->seg == 0x64 || y->seg == 0x65)) return 0;
    if ((op >= 0x88 && op <= 0x8b) || op == 0x8d || ((op == 0xc6 || op == 0xc7) && y->reg == 0)) return 1;
    if ((op >= 0x90 && op <= 0x99) || (op >= 0xb0 && op <= 0xbf) || (op >= 0x50 && op <= 0x5f)) return 1;
    if (op == (OP_0F | 0xb6) || op == (OP_0F | 0xb7) || op == (OP_0F | 0xbe) || op == (OP_0F | 0xbf)) return 1;
    if (op >= (OP_0F | 0xc8) && op <= (OP_0F | 0xcf)) return 1;
    return op == 0xf7 && y->reg == 2;      /* not */
}
static unsigned long g_st_fusion_far;
static int g_sw_skip, g_sw_skipped;   /* dead store: leave out the next 32-bit store (save_p) / it was left out */
static int g_no_dead;                 /* without ROSETTA_DEAD=1 every store to a stack slot is done at once: leaving
                                         out dead ones removes instructions but gains nothing on the G4 (stores run
                                         beside the dependency chain) */
static int g_dead_log;                /* ROSETTA_DEAD_LOG: report where a left-out store is written after all */
static int g_dead_window = 8, g_dead_writes = 1;   /* ROSETTA_DEAD_WINDOW=n,w: instructions to the overwrite, other
                                                       slot writes in between (tests/stack_fw.c: large, to write late) */
static int g_no_jumptab;            /* ROSETTA_NO_JUMPTAB: switch tables through the general hash table */
static void sw_all_drop(void);
static int sw_slot(const Insn *b, int32_t *off);
static int sw_find(int32_t off);
static void sw_fetch(int k, int t);
static void sw_const(int32_t off, uint32_t w);
static void sw_guest(int32_t off, int r);
static void sw_guest_save(int32_t off, int r);
static int sw_host(int32_t off, int q);
static int g_sw_arg_copy = 1;          /* ROSETTA_SW_ARG=0: do not remember arguments on the stack as a copy */
static int g_sw_aesp_ok, g_sw_aebp_ok;
static int32_t g_sw_aesp, g_sw_aebp;
static int g_sw_esp_ok, g_sw_ebp_ok;    /* esp/ebp after the last instruction (stack forwarding) */
static int32_t g_sw_esp, g_sw_ebp;
static int fw_find(const Insn *b, int sz);
static void fw_write(const Insn *b, int sz);
static int fw_new(const Insn *b, int sz);
static int g_no_pic;                  /* ROSETTA_NO_PIC: call +0 like any call */
static int g_no_ret;                /* ROSETTA_NO_WRITEBACK: always write stack slots to memory immediately */
static int g_no_budget;               /* ROSETTA_NO_BUDGET: only accesses that run away from the stack */
static int g_no_close;           /* ROSETTA_NO_CLOSE: falling through into the block start does not count as a loop */
static unsigned long g_st_closed;
static int g_no_inner;                /* ROSETTA_NO_INNER: conditional jumps within the block always through side exits */
static int g_no_inner_ret = 1;      /* ROSETTA_INNER_RET=1: backward inner jumps too (inner loops). Off:
                                           slower on the G4 (separate blocks have aligned heads, and the per-address
                                           2-bit prediction predicts separate copies of the branch better) */
static unsigned long g_st_inner;
static int g_no_chain;              /* ROSETTA_NO_CHAIN: do not take side exits through a conditional jump at the target */
static int g_ic_ways = 4;               /* ROSETTA_IC_WAYS: entries in the inline cache of indirect jumps (1..8) */
/* Variants: block starting at a call site (call through a pointer) for one fixed target, with the call inlined. The
   call site learns up to VAR_TARGETS targets (g_targets); its block inlines the predicted one and compares the
   others, a hit goes to the variant through an exit stub (g_var). New targets: block rebuilt; from the fifth: generic. */
#define VAR_N 512
#define VAR_TARGETS 4
static struct { uint32_t loc, target, *code; } g_var[VAR_N];
typedef struct { uint32_t loc; int n; uint32_t target[VAR_TARGETS]; } TargetList;
static TargetList g_targets[512];
static TargetList *targets_find(uint32_t loc);
static int g_var_active;                 /* translate is currently generating a variant for (g_var_loc, g_var_target) */
static uint32_t g_var_loc, g_var_target;
static int g_no_variants;            /* ROSETTA_NO_VARIANTS: second target of a prediction -> generic call */
static int g_no_precompute;           /* ROSETTA_NO_PRECOMPUTE: predict the call target only from unchanged registers */
static int g_no_xhost;                /* ROSETTA_NO_XHOST: xmm always in Intel format at block boundaries */
static int g_no_fw_stack;            /* ROSETTA_NO_FW_STACK: floating-point forwarding compares only address forms */
static uint32_t g_codepages[1u << 15]; /* bitmap of the 4 KB guest pages with translated code */
static uint32_t g_newpages[1u << 15];  /* the same for the blocks translated in this run (not from the translation cache) */
#define NEED_N 2048                      /* remembered flag liveness per jump target (need) */
static uint32_t g_need_eip[NEED_N];
static uint8_t g_need_w[NEED_N];
static uint32_t g_el_eip[1024];         /* remembered embed_worth per target */
static uint8_t g_el_w[1024];
static int g_once_flushed;
static int g_no_pageexact;          /* ROSETTA_NO_PAGEEXACT: every mprotect/munmap discards the whole cache */
static int g_cmpsb_on = 1;              /* ROSETTA_NO_CMPSB: interpret repz cmpsb in the block again */
static unsigned long g_st_needed_dec;
static int g_no_exact_sz;             /* ROSETTA_NO_EXACT_SZ: each store discards forwarding entries over 16 bytes */
static int g_no_pause = 1;            /* ROSETTA_PAUSE=1: stack slots stay dormant in inlined calls. Off: qsort
                                           at most +3.6 % (1 slot), calls -8 % (the slots take r28..r31 away from
                                           stack forwarding, which uses them for argument and ebp copies) */
static int g_pause_slots = 1;         /* ROSETTA_PAUSE_SLOTS: slots in blocks with dormant calls */
static unsigned long g_st_flush_saved;
static int g_no_setcc_cr = 1;         /* ROSETTA_SETCC_CR=1: setcc after a compare with memory data through mfcr. Off:
                                           no gain in qsort */
static unsigned long g_st_variants, g_st_var_targets;
static int g_no_stub_direct;        /* ROSETTA_NO_STUB_DIRECT: string stubs through their block */
static unsigned long g_st_stub_direct;
static int g_no_fpahead = 1;            /* ROSETTA_FPAHEAD=1: hoist floating-point loads within the block. Off: no measurable gain
                                           (n-body, matrix, bench_float unchanged; the 7447A overlaps independent load
                                           sequences by itself, the stvx -> lfd penalty remains) */
static int g_cont;               /* ROSETTA_CONT=1: continue in the block after calls in loops, =2: after all calls.
                                           Off: no gain in loops; everywhere bench_control_flow +2 %, but startup +17 %
                                           (more code is translated that may never run after the call) */
static unsigned long g_st_cont;
static unsigned long g_st_chain;

static uint32_t *p;                     /* write pointer */
static inline void e(uint32_t w) { *p++ = w; }

static void load_imm(int r, uint32_t v)
{
    if ((int32_t)v >= -32768 && (int32_t)v < 32768) e(LI(r, v));
    else {
        e(LIS(r, v >> 16));
        if (v & 0xffff) e(ORI(r, r, v & 0xffff));
    }
}

/* constants in loops: immediates that every iteration would otherwise reload, in free registers (r28..r31) */
static int g_k_n, g_k_reg[4];
static uint32_t g_k_val[4];

static int const_reg(uint32_t v)
{
    int k;
    for (k = 0; k < g_k_n; k++) if (g_k_val[k] == v) return g_k_reg[k];
    return -1;
}

/* value v in a register: constant register or t */
static int imm_reg(uint32_t v, int t)
{
    int c = const_reg(v);
    if (c >= 0) return c;
    load_imm(t, v);
    return t;
}

/* Intel register r = v */
static void reg_imm(int r, uint32_t v)
{
    int c = const_reg(v);
    if (c >= 0) e(MR(r, c)); else load_imm(r, v);
}

/* t = a + d (a not r0) */
static void addi_after(int t, int a, int32_t d)
{
    if (d == 0) { if (t != a) e(MR(t, a)); return; }
    if (d >= -32768 && d < 32768) { e(ADDI(t, a, d)); return; }
    e(ADDIS(t, a, (uint32_t)(d + 0x8000) >> 16));
    if (d & 0xffff) e(ADDI(t, t, d & 0xffff));
}

static void b_to(void *target) { e(B_((uint32_t)(uintptr_t)target - (uint32_t)(uintptr_t)p)); }

/* b at from reaches to (+-32 MB) */
static int b_reaches(const uint32_t *from, const uint32_t *to)
{
    int32_t d = (int32_t)((uint32_t)(uintptr_t)to - (uint32_t)(uintptr_t)from);
    return d >= -0x2000000 && d < 0x2000000;
}

/* the exits of the half that holds at */
static int half_of(const uint32_t *at) { return g_half2 && at >= g_half2; }
static uint32_t *exit_near(const uint32_t *at) { return half_of(at) ? g_exit2 : g_exit; }
static uint32_t *link_near(const uint32_t *at) { return half_of(at) ? g_link2 : g_link; }
static uint32_t *interrupt_near(const uint32_t *at) { return half_of(at) ? g_interrupt2 : g_interrupt; }

/* jump to target from anywhere: four words at at (r0, ctr) */
static void far_jump(uint32_t *at, const uint32_t *target)
{
    uint32_t t = (uint32_t)(uintptr_t)target;
    at[1] = ORI(0, 0, t & 0xffff);
    at[2] = MTCTR(0);
    at[3] = BCTR;
    at[0] = LIS(0, t >> 16);
    sys_icache_invalidate(at, 16);
}

/* four words in the pool of the half that holds at (0: full) */
static uint32_t *pool_take(const uint32_t *at)
{
    int h = half_of(at);
    uint32_t *end = h ? g_cache_end : g_half2;
    if (end - g_pool[h] < 4) return 0;
    g_pool[h] += 4;
    return g_pool[h] - 4;
}

/* room for n words at g_free: the rest of the first half stays empty when it does not fit there any more; the second
   half full: flush */
static void flush_full(void);
static void room(uint32_t n)
{
    if (g_free < g_half_end[0]) {
        if ((uint32_t)(g_half_end[0] - g_free) >= n) return;
        g_free = g_half2_code;
    }
    if (g_free > g_half_end[1] || (uint32_t)(g_half_end[1] - g_free) < n) flush_full();
}

static void bc_set(uint32_t *bc, uint32_t *target)
{
    *bc = (*bc & 0xffff0003u) | (((uint32_t)(uintptr_t)target - (uint32_t)(uintptr_t)bc) & 0xfffc);
}

static void range_set(void);
static void xf_all_done(void);
static int g_sl_wb_now;               /* code generation is in the loop part with deferred write-back */
static int g_sl_budget;                 /* this loop part counts iterations in CTR */

static void c_call(void *f)
{
    int ctr = g_sl_wb_now && g_sl_budget;
    xf_all_done();                   /* no pending lfd across a C call */
    fw_c_call();                      /* f2..f13 are volatile for C (host copies stay with g_c_pure) */
    sw_all_drop();                      /* C code (interpreter) can change stack slots and guest registers */
    if (ctr) { e(MFCTR(0)); e(STW(0, OFF(budget), RCPU)); }    /* the iteration budget survives the call */
    load_imm(12, (uint32_t)(uintptr_t)f);
    e(MTCTR(12));
    e(BCTRL);
    if (ctr) { e(LWZ(0, OFF(budget), RCPU)); e(MTCTR(0)); }
    range_set();                   /* r2 is volatile */
}

/* ---------- Samples ---------- */

static uint16_t *g_prof;                /* hits per word in the cache */
static unsigned long g_prof_total, g_prof_outside;
static struct { uint32_t pc, n; } g_prof_c[4096];

static void prof_signal(int sig, siginfo_t *si, void *k)
{
    ucontext_t *u = (ucontext_t *)k;
    uint32_t pc, h;
    (void)sig; (void)si;
#if __DARWIN_UNIX03
    pc = u->uc_mcontext->__ss.__srr0;
#else
    pc = u->uc_mcontext->ss.srr0;
#endif
    g_prof_total++;
    if (pc >= (uint32_t)(uintptr_t)g_cache && pc < (uint32_t)(uintptr_t)g_cache_end) {
        uint32_t i = (pc - (uint32_t)(uintptr_t)g_cache) >> 2;
        if (g_prof[i] < 65535) g_prof[i]++;
        return;
    }
    g_prof_outside++;
    for (h = (pc >> 2) & 4095; g_prof_c[h].pc && g_prof_c[h].pc != pc; h = (h + 1) & 4095) ;
    g_prof_c[h].pc = pc;
    g_prof_c[h].n++;
}

static void prof_start(void)
{
    struct sigaction sa;
    struct itimerval t;
    g_prof = calloc(CACHE_SIZE / 4, sizeof *g_prof);
    if (!g_prof) return;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = prof_signal;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, 0);
    t.it_interval.tv_sec = 0;
    t.it_interval.tv_usec = 1000;
    t.it_value = t.it_interval;
    setitimer(ITIMER_PROF, &t, 0);
}

/* floating-point registers: f14..f21 hold xmm0..7 (low element), f22 = 2^52+2^31, f23 = 2^31 */
#define XF(n) (14 + (n))
#define F_MAGIC 22
#define F_2POW31 23
#define TMP(i) (64 + 8 * (i))            /* scratch area in the stack frame: 0 and 1 fixed (read back immediately), 2..12 round-robin */
static int g_tmp;                        /* round-robin slots: no wait for the previous lfd of the same slot */
static const double g_fconst[2] = { 4503601774854144.0, 2147483648.0 };

/* ---------- Entry and exit ---------- */

static void framecode_build(void)
{
    int i;
    p = g_cache;
    g_entry = (void (*)(Cpu *, uint32_t *))(void *)p;
    e(MFLR(0)); e(STW(0, 8, 1)); e(MFCR(0)); e(STW(0, 4, 1));
    e(STWU(1, -FRAME_SIZE, 1));
    e(STMW(13, FRAME_SIZE - 76, 1));
    /* VRSAVE: the generated code uses v0..v7 (SSE); on a context switch the system saves only the declared registers */
    e(MFSPR(0, SPR_VRSAVE)); e(STW(0, FRAME_SIZE - 80, 1)); e(ORIS(0, 0, 0xff00)); e(MTSPR(SPR_VRSAVE, 0));
    for (i = 14; i <= 31; i++) e(STFD(i, FSAFE + 8 * (i - 14), 1));
    load_imm(12, (uint32_t)(uintptr_t)g_fconst);
    e(LFD(F_MAGIC, 0, 12)); e(LFD(F_2POW31, 8, 12));
    e(MR(RCPU, 3));
    /* always two instructions: the prologue length must be the same in every run (the translation cache jumps into
       g_exit), but g_table is at a different place each time */
    e(LIS(RTAB, ((uint32_t)(uintptr_t)g_table + 0x8000) >> 16));
    e(ADDI(RTAB, RTAB, (uint32_t)(uintptr_t)g_table & 0xffff));
    for (i = 0; i < 8; i++) e(LWZ(R(i), OFF(r[i]), RCPU));
    e(LWZ(RSHADOW, OFF(shadow), RCPU));
    e(MTCTR(4));
    e(BCTR);
    g_exit = p;
    for (i = 0; i < 8; i++) e(STW(R(i), OFF(r[i]), RCPU));
    e(STW(RSHADOW, OFF(shadow), RCPU));
    e(STW(3, OFF(eip), RCPU));
    e(STW(4, OFF(jit_kind), RCPU));
    e(LWZ(0, FRAME_SIZE - 80, 1)); e(MTSPR(SPR_VRSAVE, 0));
    for (i = 14; i <= 31; i++) e(LFD(i, FSAFE + 8 * (i - 14), 1));
    e(LMW(13, FRAME_SIZE - 76, 1));
    e(ADDI(1, 1, FRAME_SIZE));
    e(LWZ(0, 8, 1)); e(MTLR(0));
    e(LWZ(0, 4, 1)); e(MTCRF(0));
    e(BLR);
    g_link = p;                    /* "bl link" from an exit stub: r4 = exit stub + 12 */
    e(MFLR(4));
    b_to(exit_near(p));
    g_interrupt = p;                /* "bl interrupt" at a halted block start: r3 = this start */
    e(MFLR(3));
    e(ADDI(3, 3, -4));
    e(LI(4, KIND_INTERRUPT));
    b_to(exit_near(p));
    /* code from the next page on: the translation cache is mapped there with mmap */
    g_cache_start = (uint32_t *)(((uintptr_t)p + 4095) & ~(uintptr_t)4095);
    sys_icache_invalidate(g_cache, (size_t)((uint8_t *)p - (uint8_t *)g_cache));
    /* second half: its own exits (to the prologue's), then code; both halves end in their pools */
    g_half2 = g_cache + HALF_WORDS;
    p = g_half2;
    g_exit2 = p;                    /* r0 and ctr are free here: the prologue's exit stores neither */
    far_jump(p, g_exit);
    p += 4;
    g_link2 = p;
    e(MFLR(4));
    b_to(g_exit2);
    g_interrupt2 = p;
    e(MFLR(3));
    e(ADDI(3, 3, -4));
    e(LI(4, KIND_INTERRUPT));
    b_to(g_exit2);
    sys_icache_invalidate(g_half2, (size_t)((uint8_t *)p - (uint8_t *)g_half2));
    g_half2_code = (uint32_t *)(((uintptr_t)p + 4095) & ~(uintptr_t)4095);
    g_half_end[0] = g_half2 - POOL_WORDS;
    g_half_end[1] = g_cache_end - POOL_WORDS;
    if (getenv("ROSETTA_HALF1_KB")) {        /* testing: the first half ends early, code goes into the second soon */
        uint32_t *e1 = g_cache_start + (uint32_t)atoi(getenv("ROSETTA_HALF1_KB")) * 256u;
        if (e1 > g_cache_start && e1 < g_half_end[0]) g_half_end[0] = e1;
    }
}

/* ---------- Interrupting ----------
 * A loop in translated code does not pass the dispatcher: if another thread or a signal is waiting, the watchdog
 * (thread.c) halts the thread by replacing the first instruction of every block start with "bl interrupt". Every
 * loop enters a block start at some point (loops within a block through their budget, the others through chained
 * jumps, calls, inline caches), and at a block start the guest state is complete (as when chaining). The dispatcher
 * restores everything. Race with the translator (map, code): the watchdog writes only while the thread is in
 * translated code; Dekker pair g_in_code / g_patch_wants (one core, thread switches order the memory accesses). */
volatile int g_in_code, g_patch_wants, g_patched;
volatile unsigned g_dispatcher_rounds;
static struct { uint32_t *loc, prev, eip; } *g_up;
static int g_nup, g_up_max;
/* Loop heads behind a prologue: the backward jump skips the block start; to halt, the head is redirected to its
   own exit (writes back the state at the head, then eip = block start) */
static struct { uint32_t *head, *exit_ptr; } *g_head;
static int g_nhead, g_head_max;

static void head_note(uint32_t *head, uint32_t *exit_ptr)
{
    if (g_nhead >= g_head_max) {
        int n = g_head_max ? 2 * g_head_max : 256;
        void *z = realloc(g_head, (size_t)n * sizeof *g_head);
        if (!z) return;
        g_head = z;
        g_head_max = n;
    }
    g_head[g_nhead].head = head;
    g_head[g_nhead].exit_ptr = exit_ptr;
    g_nhead++;
}
static unsigned long g_st_interrupted, g_st_ub_starts;

/* replace the first instruction at o with fresh (remember the old one); fresh = 0: "bl interrupt" */
static void block_starts_halt(uint32_t *o, uint32_t eip, uint32_t fresh)
{
    if (o < g_cache_start || o >= g_free || g_nup >= g_up_max) return;
    if (!fresh) fresh = BL_((uint32_t)(uintptr_t)interrupt_near(o) - (uint32_t)(uintptr_t)o);
    if (*o == fresh) return;                  /* two entries, one block */
    g_up[g_nup].loc = o;
    g_up[g_nup].prev = *o;
    g_up[g_nup].eip = eip;
    g_nup++;
    *o = fresh;
    sys_icache_invalidate(o, 4);
}

/* Watchdog (without the thread lock): bind the running thread to the next block start. 1: done */
/* Debugging (ROSETTA_STATE=1): SIGUSR2 halts the running guest thread at the next block start, the dispatcher
   shows eip, stack and call chain (ebp) - for programs that hang or spin */
static volatile int g_state_wish;
static int word_read(uint32_t addr, uint32_t *w);

static void state_one(Cpu *c, const char *label)
{
    uint32_t bp = c->r[EBP], w, n, s = c->r[ESP];
    int t;
    fprintf(stderr, "%s eip %08x eax %08x ecx %08x edx %08x esp %08x ebp %08x | [esp]", label, c->eip, c->r[EAX], c->r[ECX],
            c->r[EDX], s, bp);
    for (t = 0; t < 4 && word_read(s + 4 * (uint32_t)t, &w); t++) fprintf(stderr, " %08x", w);
    fprintf(stderr, "\n  chain:");
    for (t = 0; t < 24 && bp && !(bp & 3) && word_read(bp + 4, &w); t++) {
        fprintf(stderr, " %08x", w);
        if (!word_read(bp, &n) || n <= bp) break;
        bp = n;
    }
    fprintf(stderr, "\n");
}

/* the running thread, then every other guest thread as it was when it last left translated code (blocked in a
   system call or waiting for the lock) */
static void state_short(Cpu *c)
{
    int k;
    g_state_wish = 0;
    state_one(c, "STATE");
    for (k = 0; k < g_nfcpu; k++)
        if (g_fcpu[k] != c) {
            char label[32];
            snprintf(label, sizeof label, "  THREAD %d", k);
            state_one(g_fcpu[k], label);
        }
}

static void state_signal(int sig)
{
    (void)sig;
    g_state_wish = 1;
    jit_interrupt();
}

int jit_interrupt(void)
{
    uint32_t i;
    int r = 0;
    g_patch_wants = 1;
    if (g_in_code && !g_patched && g_cache && g_interrupt) {
        int need = (int)g_nused + VAR_N + g_nhead;
        if (g_up_max < need) {
            void *n = realloc(g_up, (size_t)need * sizeof *g_up);
            if (n) { g_up = n; g_up_max = need; }
        }
        g_nup = 0;
        for (i = 0; i < g_nused; i++) {
            uint32_t h = g_inuse[i];
            if (g_map[h].code && g_map[h].eip && g_map[h].eip != 0xffffffffu) block_starts_halt(g_map[h].code, g_map[h].eip, 0);
        }
        for (i = 0; i < VAR_N; i++)
            if (g_var[i].code && g_var[i].loc != 0xffffffffu) block_starts_halt(g_var[i].code, g_var[i].loc, 0);
        for (i = 0; i < (uint32_t)g_nhead; i++)
            block_starts_halt(g_head[i].head, 0, B_((uint32_t)(uintptr_t)g_head[i].exit_ptr - (uint32_t)(uintptr_t)g_head[i].head));
        g_patched = 1;
        g_st_interrupted++;
        g_st_ub_starts += (unsigned long)g_nup;
        r = 1;
    }
    g_patch_wants = 0;
    return r;
}

/* Dispatcher (lock held, not in code): restore block starts (in reverse order: duplicates last) */
static void unpatch(void)
{
    int i;
    for (i = g_nup - 1; i >= 0; i--) {
        *g_up[i].loc = g_up[i].prev;
        sys_icache_invalidate(g_up[i].loc, 4);
    }
    g_nup = 0;
    g_patched = 0;
}

static uint32_t interrupt_eip(uint32_t host)
{
    int i;
    for (i = 0; i < g_nup; i++) if ((uint32_t)(uintptr_t)g_up[i].loc == host) return g_up[i].eip;
    return 0;
}

static void profile_harvest(void);

/* Like g_codepages, but per block: the pages it was translated from as up to DEP_SPANS spans. A remapped range then
   discards only the blocks that rely on it instead of the whole cache: a library that dyld places elsewhere than
   in the run that wrote the translation cache costs only its own blocks (and those that inlined code from it).
   Records follow the order of the code (g_free only grows between flushes), so a binary search by offset finds them */
#define DEP_SPANS 3
#define DEP_ALL 0xffffffffu             /* spans merged beyond 16 MB: the block counts as relying on every page */
typedef struct {
    uint32_t eip, code;                 /* code: byte offset in the cache; bit 0 discarded, bit 1 marked (invalidation) */
    uint32_t span[DEP_SPANS];           /* first page << 12 | pages - 1; 0 unused */
} BlockDep;
typedef char dep_five_words[sizeof(BlockDep) == 20 ? 1 : -1];     /* 5 words each in the translation cache file */
static BlockDep *g_dep;
static uint32_t g_ndep, g_dep_max;
static int g_dep_lost;                  /* a record could not be stored: remapping flushes everything again */
#define INVAL_N 64
static struct { uint32_t a, len; } g_inval[INVAL_N];     /* remapped ranges waiting for the dispatcher (g_flush 2) */
static int g_ninval, g_no_partial;
static int g_no_probe;                  /* ROSETTA_NO_PROBE: read probes through vm_read_overwrite (safe_read) */
static unsigned long g_st_inval_ranges, g_st_inval_blocks, g_st_inval_loaded;

static void flush(void)
{
    uint32_t i;
    if (g_prof) {
        profile_harvest();
        memset(g_prof + (g_cache_start - g_cache), 0, (size_t)(g_free - g_cache_start) * sizeof *g_prof);
    }
    for (i = 0; i < g_nused; i++) {
        uint32_t h = g_inuse[i];
        g_map[h].eip = 0;
        g_map[h].code = 0;
    }
    g_nused = 0;
    for (i = 0; i < g_tab_n; i++) {     /* used slots of the jump table */
        g_table[g_tab_slot[i] * 2] = 0;
        g_table[g_tab_slot[i] * 2 + 1] = 0;
    }
    if (g_tab_full) memset(g_table, 0, TABLE * 8);
    g_tab_n = 0;
    g_tab_full = 0;
    g_free = g_cache_start;
    g_pool[0] = g_half_end[0];
    g_pool[1] = g_half_end[1];
    g_nhead = 0;                        /* loop heads belong to the discarded code */
    if (g_once_flushed) {             /* the first time everything is still empty (saves 48 page faults, ~0.5 ms) */
        cpool_flush();                 /* constants belong to the discarded code */
        vl_flush();                    /* likewise rings and prefetching sites */
        memset(g_var, 0, sizeof g_var); /* and variants */
        memset(g_need_eip, 0, sizeof g_need_eip);
        memset(g_el_eip, 0, sizeof g_el_eip);
        memset(g_codepages, 0, sizeof g_codepages);
        memset(g_newpages, 0, sizeof g_newpages);
    }
    g_once_flushed = 1;
    g_ndep = 0;                         /* records of the discarded blocks */
    g_dep_lost = 0;
    g_ninval = 0;                       /* waiting ranges are covered */
    /* Return stack: its host addresses point into discarded code. Back to the start of the ring: before it lies the
       sentinel entry that matches no ret (old returns then go through the table). All threads: the
       others wait for the lock, outside the translated code */
    {
        int i;
        for (i = 0; i < g_nfcpu; i++) g_fcpu[i]->shadow = g_fcpu[i]->shadow_base;
    }
    g_flush = 0;
    g_st_flushes++;
}

/* the code region is full: everything goes, but the translation cache file still fits the program (it is loaded
   again next time; only a flush for other reasons deletes it) */
static unsigned long g_st_full_flushes;
static void flush_full(void)
{
    g_st_full_flushes++;
    flush();
}

/* guest pages that were translated from (code, predicted constants): only a change to them discards the cache */
static void page_note(uint32_t a)
{
    g_codepages[a >> 17] |= 1u << ((a >> 12) & 31);
    g_newpages[a >> 17] |= 1u << ((a >> 12) & 31);
}

/* add the page of a to the spans: into a span within 64 KB, else a new span, else the nearest one grows */
static void dep_note(uint32_t *sp, uint32_t a)
{
    uint32_t pg = a >> 12, i, best = 0, bestd = 0xffffffffu, lo, hi;
    if (sp[0] == DEP_ALL) return;
    for (i = 0; i < DEP_SPANS && sp[i]; i++) {
        lo = sp[i] >> 12;
        hi = lo + (sp[i] & 4095);
        if (pg >= lo && pg <= hi) return;
        if ((pg < lo ? lo - pg : pg - hi) < bestd) { bestd = pg < lo ? lo - pg : pg - hi; best = i; }
    }
    if (i < DEP_SPANS && bestd > 16) { sp[i] = pg << 12; return; }
    lo = sp[best] >> 12;
    hi = lo + (sp[best] & 4095);
    if (pg < lo) lo = pg; else hi = pg;
    if (hi - lo > 4095) { sp[0] = DEP_ALL; for (i = 1; i < DEP_SPANS; i++) sp[i] = 0; return; }
    sp[best] = lo << 12 | (hi - lo);
}

static void dep_add(uint32_t eip, const uint32_t *code, const uint32_t *sp)
{
    int i;
    if (g_ndep >= g_dep_max) {
        uint32_t n = g_dep_max ? 2 * g_dep_max : 4096;
        void *z = realloc(g_dep, (size_t)n * sizeof *g_dep);
        if (!z) { g_dep_lost = 1; return; }
        g_dep = z;
        g_dep_max = n;
    }
    g_dep[g_ndep].eip = eip;
    g_dep[g_ndep].code = (uint32_t)((const uint8_t *)code - (const uint8_t *)g_cache);
    for (i = 0; i < DEP_SPANS; i++) g_dep[g_ndep].span[i] = sp[i];
    g_ndep++;
}

/* record of the block starting at code, or 0 */
static BlockDep *dep_find(const uint32_t *code)
{
    uint32_t off = (uint32_t)((const uint8_t *)code - (const uint8_t *)g_cache), lo = 0, hi = g_ndep;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        if ((g_dep[m].code & ~3u) < off) lo = m + 1; else hi = m;
    }
    return lo < g_ndep && (g_dep[lo].code & ~3u) == off ? &g_dep[lo] : 0;
}

/* the block relies on a page in p0..p1 */
static int dep_hits(const BlockDep *d, uint32_t p0, uint32_t p1)
{
    int i;
    if (d->span[0] == DEP_ALL) return 1;
    for (i = 0; i < DEP_SPANS && d->span[i]; i++) {
        uint32_t lo = d->span[i] >> 12, hi = lo + (d->span[i] & 4095);
        if (lo <= p1 && hi >= p0) return 1;
    }
    return 0;
}

static int pages_hit_in(const uint32_t *pages, uint32_t a, uint32_t len)
{
    uint32_t s, e2;
    if (!len) return 0;
    s = a >> 12;
    e2 = (uint32_t)(((uint64_t)a + len - 1) >> 12);
    for (; s <= e2; s++) {
        if (!(s & 31) && s + 31 <= e2 && !pages[s >> 5]) { s += 31; continue; }     /* whole word empty */
        if (pages[s >> 5] >> (s & 31) & 1) return 1;
    }
    return 0;
}

static int pages_hit(uint32_t a, uint32_t len) { return pages_hit_in(g_codepages, a, len); }

static int g_cache_show;
static void jit_range_changed(uint32_t a, uint32_t len, const char *why);

static void ranges_forget(uint32_t a, uint32_t len);

/* guest remapped [a, a + len) or changed its protection */
void jit_flush_schedule(uint32_t a, uint32_t len)
{
    ranges_forget(a, len);              /* the region cache of page protections */
    memset(g_need_eip, 0, sizeof g_need_eip);     /* remembered results depended on page protection */
    memset(g_el_eip, 0, sizeof g_el_eip);
    if (g_no_pageexact || pages_hit(a, len)) jit_range_changed(a, len, "mapping/protection");
    else g_st_flush_saved++;
}

/* translated code relies on [a, a + len): discard the blocks concerned at the next round of the dispatcher (only
   they, unless ROSETTA_NO_PARTIAL, page exactness is off or too many ranges are waiting: then everything) */
static void jit_range_changed(uint32_t a, uint32_t len, const char *why)
{
    if (g_flush == 3) ;                 /* a flush for room is waiting: it covers the range */
    else if (g_flush != 1 && !g_no_pageexact && !g_no_partial && !g_dep_lost && g_ninval < INVAL_N) {
        g_inval[g_ninval].a = a;
        g_inval[g_ninval].len = len;
        g_ninval++;
        g_flush = 2;
    } else g_flush = 1;
    if (g_cache_show) fprintf(stderr, "rosetta: %s: %s %08x+%x (eip %08x)\n", g_flush == 1 ? "flush" : "invalidate", why, a, len,
                              g_cpu ? g_cpu->eip : 0);
}

/* Return ring (one per thread): aligned to its size, preceded by the sentinel entry (matches no
   return; ret at the start of the ring then goes through the table). 0: no space */
static uint32_t ring_new(void)
{
    vm_address_t s = 0;     /* 4 KB more: there is always allocated memory before the ring for the sentinel entry */
    uint32_t b;
    if (vm_allocate(mach_task_self(), &s, 2 * (64u * 1024 * 8) + 4096, TRUE) != KERN_SUCCESS) return 0;
    b = ((uint32_t)s + 4096 + (64u * 1024 * 8) - 1) & ~(64u * 1024 * 8 - 1);
    if (g_exit) {
        ((uint32_t *)(uintptr_t)b)[-2] = 0xffffffffu;
        ((uint32_t *)(uintptr_t)b)[-1] = (uint32_t)(uintptr_t)g_exit;
    }
    return b;
}

static int jit_create(void)
{
    vm_address_t a = CACHE_ADDR;
    if (g_cache) return 0;
    if (vm_allocate(mach_task_self(), &a, CACHE_SIZE, FALSE) != KERN_SUCCESS &&
        vm_allocate(mach_task_self(), &a, CACHE_SIZE, TRUE) != KERN_SUCCESS) return -1;
    mprotect((void *)a, CACHE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC);
    g_cache = (uint32_t *)a;
    g_cache_end = g_cache + CACHE_SIZE / 4;
    g_table = calloc(TABLE, 8);
    g_map = calloc(1u << g_map_bits, sizeof *g_map);
    g_inuse = calloc(1u << g_map_bits, sizeof *g_inuse);
    if (!g_table || !g_map || !g_inuse) return -1;
    if (!(g_shadow_base = ring_new())) return -1;
    g_careful = getenv("ROSETTA_CAREFUL") ? (atoi(getenv("ROSETTA_CAREFUL")) ? atoi(getenv("ROSETTA_CAREFUL")) : 7) : 0;   /* 1 call, 2 ret, 4 jump tables */
    g_no_fusion = getenv("ROSETTA_NO_FUSION") != 0;
    g_no_narrow = getenv("ROSETTA_NO_NARROW") != 0;
    g_no_slots = getenv("ROSETTA_NO_SLOTS") != 0;
    g_poison = getenv("ROSETTA_POISON") != 0;   /* merged floating-point compares poison the flags in memory */
    if (getenv("ROSETTA_STATS")) { g_step_fn = step_counted; g_vt_on = atoi(getenv("ROSETTA_STATS")) > 1 ? 2 : 1; }
    if (getenv("ROSETTA_ALIGN")) g_align = atoi(getenv("ROSETTA_ALIGN"));
    g_no_pic = getenv("ROSETTA_NO_PIC") != 0;
    g_no_const = getenv("ROSETTA_NO_CONST") != 0;
    g_no_prefetch = getenv("ROSETTA_NO_PREFETCH") != 0;
    g_no_fw = getenv("ROSETTA_NO_FW") != 0;
    g_no_pd = getenv("ROSETTA_NO_PD") != 0;
    g_no_xfh = getenv("ROSETTA_NO_XFH") != 0;
    g_no_native = getenv("ROSETTA_NO_NATIVE") != 0;
    g_no_embed = getenv("ROSETTA_NO_EMBED") != 0;
    g_no_sw = getenv("ROSETTA_NO_SW") != 0;
    g_no_sw2 = getenv("ROSETTA_NO_SW2") != 0;
    g_no_inplace = getenv("ROSETTA_NO_INPLACE") != 0;
    g_no_loadcopy = getenv("ROSETTA_NO_LOADCOPY") != 0;
    if (getenv("ROSETTA_LOADCOPY_DIST")) g_loadcopy_dist = atoi(getenv("ROSETTA_LOADCOPY_DIST"));
    g_no_narrowfw = getenv("ROSETTA_NO_NARROWFW") != 0;
    g_no_taken = getenv("ROSETTA_NO_TAKEN") != 0;
    g_no_far = getenv("ROSETTA_NO_FAR") != 0;
    g_no_mmx = getenv("ROSETTA_NO_MMX") != 0;
    g_no_dead = !getenv("ROSETTA_DEAD");   /* off: no measurable gain (sha1 1.10 s against 1.11 s, realworld equal) */
    g_dead_log = getenv("ROSETTA_DEAD_LOG") != 0;
    if (getenv("ROSETTA_DEAD_WINDOW")) sscanf(getenv("ROSETTA_DEAD_WINDOW"), "%d,%d", &g_dead_window, &g_dead_writes);
    g_no_ret = getenv("ROSETTA_NO_WRITEBACK") != 0;
    g_no_budget = getenv("ROSETTA_NO_BUDGET") != 0;
    g_no_close = getenv("ROSETTA_NO_CLOSE") != 0;
    g_no_inner = getenv("ROSETTA_NO_INNER") != 0;
    g_no_chain = getenv("ROSETTA_NO_CHAIN") != 0;
    if (getenv("ROSETTA_CONT")) g_cont = atoi(getenv("ROSETTA_CONT"));
    if (getenv("ROSETTA_FPAHEAD")) g_no_fpahead = !atoi(getenv("ROSETTA_FPAHEAD"));
    g_no_stub_direct = getenv("ROSETTA_NO_STUB_DIRECT") != 0;
    g_no_variants = getenv("ROSETTA_NO_VARIANTS") != 0;
    g_no_precompute = getenv("ROSETTA_NO_PRECOMPUTE") != 0;
    g_no_xhost = getenv("ROSETTA_NO_XHOST") != 0;
    g_no_fw_stack = getenv("ROSETTA_NO_FW_STACK") != 0;
    if (getenv("ROSETTA_SETCC_CR")) g_no_setcc_cr = !atoi(getenv("ROSETTA_SETCC_CR"));
    g_no_pageexact = getenv("ROSETTA_NO_PAGEEXACT") != 0;
    g_no_partial = getenv("ROSETTA_NO_PARTIAL") != 0;
    g_no_probe = getenv("ROSETTA_NO_PROBE") != 0;
    g_cmpsb_on = !getenv("ROSETTA_NO_CMPSB");
    if (getenv("ROSETTA_PAUSE")) g_no_pause = !atoi(getenv("ROSETTA_PAUSE"));
    g_no_exact_sz = getenv("ROSETTA_NO_EXACT_SZ") != 0;
    if (getenv("ROSETTA_PAUSE_SLOTS")) g_pause_slots = atoi(getenv("ROSETTA_PAUSE_SLOTS"));
    if (getenv("ROSETTA_IC_WAYS")) g_ic_ways = atoi(getenv("ROSETTA_IC_WAYS"));
    if (g_ic_ways < 1) g_ic_ways = 1;
    if (g_ic_ways > 8) g_ic_ways = 8;
    if (getenv("ROSETTA_INNER_RET")) g_no_inner_ret = !atoi(getenv("ROSETTA_INNER_RET"));
    if (getenv("ROSETTA_SW_ARG")) g_sw_arg_copy = atoi(getenv("ROSETTA_SW_ARG"));
    if (getenv("ROSETTA_MAX_INSNS")) g_max_insns = atoi(getenv("ROSETTA_MAX_INSNS"));
    if (g_max_insns < 8) g_max_insns = 8;
    if (g_max_insns > MAX_INSNS) g_max_insns = MAX_INSNS;
    g_no_jumptab = !getenv("ROSETTA_JUMPTAB");     /* brought nothing measurable in bench_control_flow: only on request */
    framecode_build();
    /* sentinel entry before the ring: matches no return (ret at the start of the ring then goes through the table) */
    ((uint32_t *)(uintptr_t)g_shadow_base)[-2] = 0xffffffffu;
    ((uint32_t *)(uintptr_t)g_shadow_base)[-1] = (uint32_t)(uintptr_t)g_exit;
    flush();
    g_st_flushes = 0;
    cache_load();
    if (getenv("ROSETTA_PROFILE")) prof_start();
    return 0;
}

/* ---------- Map ---------- */

static unsigned long g_st_find, g_st_samples;

static uint32_t *map_find(uint32_t eip)
{
    uint32_t h = MAP_H(eip), i;
    g_st_find++;
    for (i = 0; i <= g_map_mask; i++, h = (h + 1) & g_map_mask) {
        g_st_samples++;
        if (g_map[h].eip == eip && g_map[h].code) return g_map[h].code;
        if (!g_map[h].code) return 0;
    }
    return 0;
}

/* grow the map (fourfold) when it is half full: starting small saves page faults at startup */
static void map_grow(void)
{
    struct { uint32_t eip; uint32_t *code; } *prev = (void *)g_map;
    uint32_t *old_inuse = g_inuse, old_n = g_nused, i;
    void *fresh = calloc(1u << (g_map_bits + 2), sizeof *g_map), *new_b = calloc(1u << (g_map_bits + 2), sizeof *g_inuse);
    if (!fresh || !new_b) { free(fresh); free(new_b); return; }
    g_map_bits += 2;
    g_map_mask = (1u << g_map_bits) - 1;
    g_map = fresh;
    g_inuse = new_b;
    g_nused = 0;
    for (i = 0; i < old_n; i++) {
        uint32_t a = old_inuse[i], h;
        if (!prev[a].code || prev[a].eip == 0xffffffff) continue;     /* removed entries are dropped */
        for (h = MAP_H(prev[a].eip); g_map[h].code; h = (h + 1) & g_map_mask) ;
        g_map[h].eip = prev[a].eip;
        g_map[h].code = prev[a].code;
        g_inuse[g_nused++] = h;
    }
    free(prev);
    free(old_inuse);
}

static void map_add(uint32_t eip, uint32_t *code)
{
    uint32_t h;
    if (2 * (g_nused + 1) > g_map_mask + 1) map_grow();
    h = MAP_H(eip);
    while (g_map[h].code && g_map[h].eip != eip) h = (h + 1) & g_map_mask;
    if (!g_map[h].code) g_inuse[g_nused++] = h;
    g_map[h].eip = eip;
    g_map[h].code = code;
    /* into the jump table only when an indirect jump misses the target (table_add in the dispatcher): entered
       immediately, every new block touched a different one of its 128 pages (~100 page faults at startup) */
}

/* target of an indirect jump into the jump table (the slot is remembered for the flush) */
static void table_add(uint32_t eip, uint32_t *code)
{
    uint32_t t = eip & (TABLE - 1);
    if (!g_table[t * 2]) {
        if (g_tab_n < TABLE) g_tab_slot[g_tab_n++] = (uint16_t)t;
        else g_tab_full = 1;            /* list full (after many removals): clear completely at the flush */
    }
    g_table[t * 2] = eip;
    g_table[t * 2 + 1] = (uint32_t)(uintptr_t)code;
}

static void map_remove(uint32_t eip)
{
    uint32_t h = MAP_H(eip);
    while (g_map[h].code) {
        if (g_map[h].eip == eip) {
            /* entry stays occupied (chain), but no longer points to anything: retranslate */
            g_map[h].eip = 0xffffffff;
            break;
        }
        h = (h + 1) & g_map_mask;
    }
    if (g_table[(eip & (TABLE - 1)) * 2] == eip) g_table[(eip & (TABLE - 1)) * 2] = 0;
}

/* ---------- Writable pages ---------- */

/* vm_region is a Mach call (expensive): remember the last regions until the guest rearranges its memory */
#define RANGES 64                     /* program, dyld, libraries, stack, heap: 8 were not enough */
static unsigned long g_st_region, g_st_safe_read;    /* own Mach calls of the translator, failed read probes (ROSETTA_STATS) */
static struct { uint32_t start, end; int prot; } g_range[RANGES];
static int g_nrange;

/* VM_PROT_* of the region around a; -1 = not allocated */
static int page_prot(uint32_t a)
{
    vm_address_t addr = a & ~4095u;
    vm_size_t size;
    vm_region_basic_info_data_t info;
    mach_msg_type_number_t n = VM_REGION_BASIC_INFO_COUNT;
    mach_port_t obj;
    int i;
    static int latest;                 /* region hit last: almost always the same one */
    if (g_ranges_old) { memset(g_range, 0, sizeof g_range); g_ranges_old = 0; }
    if (a - g_range[latest].start < g_range[latest].end - g_range[latest].start) return g_range[latest].prot;
    for (i = 0; i < RANGES; i++)
        if (a - g_range[i].start < g_range[i].end - g_range[i].start) { latest = i; return g_range[i].prot; }
    g_st_region++;
    if (vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO, (vm_region_info_t)&info, &n, &obj))
        return -1;
    if (addr > a) return -1;
    i = g_nrange++ % RANGES;
    g_range[i].start = (uint32_t)addr;
    g_range[i].end = (uint32_t)(addr + size);
    if (g_range[i].end < g_range[i].start) g_range[i].end = 0xfffff000u;
    g_range[i].prot = info.protection;
    latest = i;
    return info.protection;
}

/* [a, a + len) remapped or protected anew: cached regions that overlap it are dropped; the others keep their
   protection (an mmap can merge them with the new range, but does not change their pages). Dropping all of them at
   every guest mmap made the translator ask vm_region again and again at startup (8 % of Calculator's first start) */
static void ranges_forget(uint32_t a, uint32_t len)
{
    int i;
    uint32_t e = a + len;
    if (!len) return;
    if (e < a) { g_ranges_old = 1; return; }
    for (i = 0; i < RANGES; i++)
        if (g_range[i].start < e && a < g_range[i].end) g_range[i].start = g_range[i].end = 0;
}

static int page_readonly(uint32_t a)
{
    int s = page_prot(a);
    return s >= 0 && (s & VM_PROT_READ) && !(s & VM_PROT_WRITE);
}

static int page_writable(uint32_t a)
{
    int s = page_prot(a);
    return s < 0 || (s & VM_PROT_WRITE);
}

/* ---------- Classifying instructions ---------- */

enum { CONT, JUMP, JUMP_COND, CALL, RET, JUMP_IND, CALL_IND };

static uint8_t cond_mask(int k)
{
    static const uint8_t m[8] = { FOF, FCF, FZF, FCF | FZF, FSF, FPF, FSF | FOF, FZF | FSF | FOF };
    return m[(k >> 1) & 7];
}

/* SSE instructions the translator does with AltiVec (gen_sse, no flags): SSE2 integer instructions on xmm registers
   and packed floats except division and square root */
/* MMX: integer instruction without prefix on mm0..mm7 (gen_sse does it on Cpu.mm like the SSE2 form on Cpu.xmm) */
static int is_mmx(const Insn *b)
{
    int o = b->op & 0xff;
    return b->op >= OP_0F && b->op <= (OP_0F | 0xff) && !b->p66 && !b->rep &&
           ((o >= 0x60 && o <= 0x7f) || o == 0xc4 || o == 0xc5 || o >= 0xd0);
}

static int mmx_can(const Insn *b)
{
    int o = b->op & 0xff;
    if (g_no_mmx || !is_mmx(b)) return 0;
    switch (o) {
    case 0x6f: case 0x7f: case 0x6e: case 0x7e: case 0x70: case 0x77:   /* movq, movd, pshufw, emms */
        return 1;
    case 0xe7: return b->mem;                                          /* movntq */
    case 0xd7: case 0xc5: return !b->mem;                              /* pmovmskb, pextrw */
    case 0xc4: return 1;                                               /* pinsrw */
    case 0x71: case 0x72: return !b->mem && (b->reg == 2 || b->reg == 4 || b->reg == 6);
    case 0x73: return !b->mem && (b->reg == 2 || b->reg == 6);         /* psrlq, psllq */
    case 0xd1: case 0xd2: case 0xe1: case 0xe2: case 0xf1: case 0xf2:  /* shifts by mm/m64 */
        return 1;
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
    case 0x68: case 0x69: case 0x6a: case 0x6b: case 0x74: case 0x75: case 0x76:
    case 0xd5: case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde: case 0xdf:
    case 0xe0: case 0xe3: case 0xe4: case 0xe5: case 0xe8: case 0xe9: case 0xea: case 0xeb: case 0xec:
    case 0xed: case 0xee: case 0xef: case 0xf5: case 0xf6: case 0xf8: case 0xf9: case 0xfa: case 0xfc:
    case 0xfd: case 0xfe:
        return 1;
    }
    return 0;
}

static int sse_can(const Insn *b)
{
    int o = b->op & 0xff, p = b->p66 && !b->rep, n = !b->p66 && !b->rep;
    if (b->op < OP_0F || b->op > (OP_0F | 0xff)) return 0;
    if (is_mmx(b)) return mmx_can(b);
    switch (o) {
    case 0x6f: case 0x7f:                   /* movdqa (66), movdqu (f3) */
        return p || (b->rep == 0xf3 && !b->p66);
    case 0xd7: case 0xc5:                   /* pmovmskb, pextrw */
        return p && !b->mem;
    case 0xe7:                              /* movntdq */
        return p && b->mem;
    case 0x70:                              /* pshufd (66), pshufhw (f3), pshuflw (f2) */
        return p || ((b->rep == 0xf2 || b->rep == 0xf3) && !b->p66);
    case 0x5b:                              /* cvtdq2ps, cvtps2dq (66), cvttps2dq (f3) */
        return n || p || (b->rep == 0xf3 && !b->p66);
    case 0x10: case 0x11:                   /* movups */
    case 0x14: case 0x15: case 0xc6:        /* unpcklps, unpckhps, shufps */
    case 0x58: case 0x59: case 0x5c: case 0x5d: case 0x5f:      /* addps, mulps, subps, minps, maxps */
        return n;
    case 0x71: case 0x72:                   /* psrlw/d, psraw/d, psllw/d with a constant */
        return p && !b->mem && (b->reg == 2 || b->reg == 4 || b->reg == 6);
    case 0x73:                              /* psrlq, psrldq, psllq, pslldq with a constant */
        return p && !b->mem && (b->reg == 2 || b->reg == 3 || b->reg == 6 || b->reg == 7);
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:     /* unpack, pack, */
    case 0x68: case 0x69: case 0x6a: case 0x6b: case 0x6c: case 0x6d:                           /* pcmpgt */
    case 0x6e: case 0x7e: case 0xc4:                                                            /* movd, pinsrw */
    case 0x74: case 0x75: case 0x76:                                                            /* pcmpeq */
    case 0xd5: case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde: case 0xdf:
    case 0xe0: case 0xe3: case 0xe4: case 0xe5: case 0xe8: case 0xe9: case 0xea: case 0xeb: case 0xec:
    case 0xed: case 0xee: case 0xef: case 0xf5: case 0xf6: case 0xf8: case 0xf9: case 0xfa: case 0xfc:
    case 0xfd: case 0xfe:                   /* add/sub (also saturating), mul, min/max, avg, sad, logic */
    case 0xd1: case 0xd2: case 0xe1: case 0xe2: case 0xf1: case 0xf2:  /* shifts by xmm/m128 */
        return p;
    }
    return 0;
}

/* returns 1 if the translator can do the instruction; reads/writes: flags; art: control flow */
static int classify(const Insn *b, uint8_t *reads, uint8_t *writes, int *kind)
{
    int op = b->op, sz = b->opsz;
    *reads = 0; *writes = 0; *kind = CONT;
    if (op >= OP_0F && sse_can(b)) return 1;
    if (op >= OP_0F && pd_can(b)) return 1;
    if (op >= OP_0F && fp_can(b)) {
        if (is_fp_cmp(b)) *writes = TRAP;
        return 1;
    }
    if (x87_can(b)) {
        if (is_x87_cmp(b)) *writes = TRAP;
        return 1;
    }
    if (op == 0xa6 && !b->seg && g_cmpsb_on) { *writes = TRAP; return 1; }     /* cmpsb, also repz/repnz (gen_cmpsb) */
    if (b->rep) return 0;                   /* also pause: see inline_possible */
    if (op < 0x40 && (op & 7) < 6) {      /* 8, 16 and 32 bit */
        *writes = TRAP;
        if ((op >> 3) == 2 || (op >> 3) == 3) *reads = FCF;
        return 1;
    }
    switch (op) {
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f:
        *writes = TRAP & ~FCF;           /* 32 and 16 bit */
        return 1;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
    case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
    case 0x68: case 0x6a:
        return sz == 4;
    case 0x69: case 0x6b: case OP_0F | 0xaf:
        if (sz != 4) return 0;
        *writes = TRAP;
        return 1;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f:
        *reads = cond_mask(op & 15); *kind = JUMP_COND; return 1;
    case OP_0F | 0x80: case OP_0F | 0x81: case OP_0F | 0x82: case OP_0F | 0x83:
    case OP_0F | 0x84: case OP_0F | 0x85: case OP_0F | 0x86: case OP_0F | 0x87:
    case OP_0F | 0x88: case OP_0F | 0x89: case OP_0F | 0x8a: case OP_0F | 0x8b:
    case OP_0F | 0x8c: case OP_0F | 0x8d: case OP_0F | 0x8e: case OP_0F | 0x8f:
        if (sz != 4) return 0;
        *reads = cond_mask(op & 15); *kind = JUMP_COND; return 1;
    case 0x80: case 0x81: case 0x83:       /* also 16 bit (stdio flags: orw/andw/cmpw on FILE._flags) */
        *writes = TRAP;
        if (b->reg == 2 || b->reg == 3) *reads = FCF;
        return 1;
    case 0x84: case 0x85: case 0xa8: case 0xa9:
        *writes = TRAP;
        return 1;
    case 0x87:                              /* also with memory (libSystem locks) */
        return sz == 4;
    case 0x88: case 0x8a: case 0xa0: case 0xa2: case 0xc6:
        return 1;
    case 0x89: case 0x8b:
        return sz == 4 || sz == 2;
    case 0xa1: case 0xa3:                   /* also movw with a fixed address (gzip's bit buffer) */
        return sz == 4 || sz == 2;
    case 0xc7:                              /* also movw $imm16 */
        return 1;
    case 0x8d:
        return sz == 4 && b->mem;
    case 0x90:
    case 0xfc: case 0xfd:                   /* cld, std: only DF in Cpu.fl (gcc -Os puts cld before every repz cmpsb) */
        return 1;
    case 0x98: case 0x99:
        return sz == 4;
    case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
        return 1;
    case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
        return sz == 4 || sz == 2;          /* also movw $imm16, %r16 */
    case 0xc1: case 0xd1:
        if (op == 0xc1 && (b->imm & 31) == 0) return sz == 4 || (sz == 2 && !b->mem);
        if (b->reg <= 1 && (sz == 4 || (sz == 2 && !b->mem))) {
            /* rol/ror (also rolw $8 of a register: ntohs): only CF and OF; if one is live, the interpreter computes */
            *writes = FCF | FOF;
            return 1;
        }
        if (!(b->reg == 4 || b->reg == 5 || b->reg == 7)) return 0;
        if (sz != 4 && sz != 2) return 0;   /* 16 bit: shrw $n of a register or a word in memory */
        *writes = TRAP;
        return 1;
    case 0xd3:                              /* count 0 leaves the flags unchanged: neither read nor write */
        return sz == 4 && (b->reg <= 1 || b->reg == 4 || b->reg == 5 || b->reg == 7);
    case 0xc0: case 0xd0:                   /* a byte (register or memory) by 1..7 (gen_shift8) */
        if (b->mem && b->reg <= 1) return 0;   /* rotating a byte in memory: interpreter */
        {
            int n = op == 0xd0 ? 1 : (int)(b->imm & 31);
            if (n == 0) return 1;
            if (n > 7) return 0;
            if (b->reg <= 1) { *writes = FCF | FOF; return 1; }
            if (!(b->reg == 4 || b->reg == 5 || b->reg == 7)) return 0;
            *writes = TRAP;
            return 1;
        }
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:   /* xchg eax, r32 */
        return sz == 4;
    case OP_0F | 0xa4: case OP_0F | 0xac:   /* shld/shrd $n (gen_double_shift) */
        if (sz != 4) return 0;
        if (b->imm & 31) *writes = TRAP;    /* count 0: nothing happens */
        return 1;
    case OP_0F | 0xa5: case OP_0F | 0xad:   /* the same with cl: as d3, neither read nor write */
        return sz == 4;
    case 0xc2: case 0xc3:
        *kind = RET; return 1;
    case 0xc9:
        return 1;
    case 0xe8:
        *kind = CALL; return 1;
    case 0xe9: case 0xeb:
        *kind = JUMP; return 1;
    case 0xf6:
        if (b->reg == 0) { *writes = TRAP; return 1; }
        return 0;
    case 0xf7:
        if (sz != 4) return 0;
        if (b->reg == 0 || b->reg == 3 || b->reg == 4 || b->reg == 5) { *writes = TRAP; return 1; }
        return b->reg == 2 || b->reg == 6 || b->reg == 7;           /* not, div, idiv: flags stay */
    case OP_0F | 0xbc: case OP_0F | 0xbd:
        if (sz != 4) return 0;
        *writes = TRAP;
        return 1;
    case 0xfe:                              /* incb, decb (CF stays) */
        if (b->reg > 1 || b->lock) return 0;
        *writes = TRAP & ~FCF;
        return 1;
    case 0xff:
        if (sz == 2 && b->reg <= 1) { *writes = TRAP & ~FCF; return 1; }  /* incw/decw (counters in structures) */
        if (sz != 4) return 0;
        switch (b->reg) {
        case 0: case 1: *writes = TRAP & ~FCF; return 1;
        case 2: *kind = CALL_IND; return 1;
        case 4: *kind = JUMP_IND; return 1;
        case 6: return 1;
        }
        return 0;
    case OP_0F | 0x18: case OP_0F | 0x19: case OP_0F | 0x1a: case OP_0F | 0x1b:
    case OP_0F | 0x1c: case OP_0F | 0x1d: case OP_0F | 0x1e: case OP_0F | 0x1f:
        return 1;
    case OP_0F | 0x77:                      /* emms: the mm registers are separate from the x87 stack, nothing to do */
        return !b->p66 && !b->rep;
    case OP_0F | 0x40: case OP_0F | 0x41: case OP_0F | 0x42: case OP_0F | 0x43:
    case OP_0F | 0x44: case OP_0F | 0x45: case OP_0F | 0x46: case OP_0F | 0x47:
    case OP_0F | 0x48: case OP_0F | 0x49: case OP_0F | 0x4a: case OP_0F | 0x4b:
    case OP_0F | 0x4c: case OP_0F | 0x4d: case OP_0F | 0x4e: case OP_0F | 0x4f:
        if (sz != 4) return 0;
        *reads = cond_mask(op & 15); return 1;
    case OP_0F | 0x90: case OP_0F | 0x91: case OP_0F | 0x92: case OP_0F | 0x93:
    case OP_0F | 0x94: case OP_0F | 0x95: case OP_0F | 0x96: case OP_0F | 0x97:
    case OP_0F | 0x98: case OP_0F | 0x99: case OP_0F | 0x9a: case OP_0F | 0x9b:
    case OP_0F | 0x9c: case OP_0F | 0x9d: case OP_0F | 0x9e: case OP_0F | 0x9f:
        *reads = cond_mask(op & 15); return 1;
    case OP_0F | 0xb6: case OP_0F | 0xb7: case OP_0F | 0xbe: case OP_0F | 0xbf:
        return sz == 4 || sz == 2;          /* also movzbw/movsbw into a 16-bit register */
    case OP_0F | 0xc8: case OP_0F | 0xc9: case OP_0F | 0xca: case OP_0F | 0xcb:
    case OP_0F | 0xcc: case OP_0F | 0xcd: case OP_0F | 0xce: case OP_0F | 0xcf:
        return 1;
    }
    return 0;
}

/* flags for any instruction (also those the interpreter does), for looking into following blocks */
static void flags_generic(const Insn *b, uint8_t *reads, uint8_t *writes, int *kind)
{
    int op = b->op;
    if (classify(b, reads, writes, kind)) return;
    *kind = CONT;
    *reads = 0; *writes = 0;
    if ((op == 0xdb || op == 0xdf) && !b->mem && (b->reg == 5 || b->reg == 6)) { *writes = TRAP; return; }  /* fcomi */
    if (op == 0xfc || op == 0xfd) return;   /* cld, std: only DF */
    if ((op >= 0xd8 && op <= 0xdf && !(op == 0xda || op == 0xdb)) ||
        (op >= (OP_0F | 0x10) && op <= (OP_0F | 0x17)) || (op >= (OP_0F | 0x28) && op <= (OP_0F | 0x2d)) ||
        (op >= (OP_0F | 0x50) && op <= (OP_0F | 0x7f)) || (op >= (OP_0F | 0xd0)) || op == 0x8f ||
        (op >= 0x91 && op <= 0x97) || op == 0x86 || op == 0x87 || op == 0x89 || op == 0x8b || op == 0xc7 || op == 0xb8)
        return;
    if (op == (OP_0F | 0x2e) || op == (OP_0F | 0x2f)) { *writes = TRAP; return; }
    if ((op & ~1) == 0xa4 || (op & ~1) == 0xaa || (op & ~1) == 0xac) return;     /* movs, stos, lods */
    *reads = TRAP;                         /* unknown: could read flags */
}

/* Which of the flags in "unresolved" can be read from eip on before they are written */
static uint8_t flags_needed(uint32_t eip, uint8_t unresolved, int depth, int *budget)
{
    uint8_t g = 0, l, s;
    Insn b;
    int kind;
    while (unresolved & ~g) {
        if (--*budget < 0) return g | unresolved;
        if (eip - COMMPAGE_BASE < COMMPAGE_SIZE) return g | unresolved;
        g_st_needed_dec++;
        if (!decode(eip, &b)) return g | unresolved;
        flags_generic(&b, &l, &s, &kind);
        g |= l & unresolved;
        unresolved &= ~s;
        if (!unresolved) break;
        switch (kind) {
        case CONT: eip += b.len; break;
        case JUMP: eip = b.eip + b.len + b.imm; break;
        case JUMP_COND:
            if (depth <= 0) return g | unresolved;
            g |= flags_needed(b.eip + b.len + b.imm, unresolved, depth - 1, budget);
            eip += b.len;
            break;
        case CALL: case CALL_IND:             /* flags do not cross call/ret/jump tables */
            return (g_careful & 1) ? (uint8_t)(g | unresolved) : g;
        case RET:
            return (g_careful & 2) ? (uint8_t)(g | unresolved) : g;
        case JUMP_IND:
            return (g_careful & 4) ? (uint8_t)(g | unresolved) : g;
        default: return g | unresolved;
        }
    }
    return g;
}

/* remember results per target (many blocks query the same targets, inner jumps a second time); only for code
   on non-writable pages, flushed with the cache (g_need_* are defined above) */
static uint8_t needed(uint32_t eip)
{
    int budget = 40;
    uint32_t h = (eip * 2654435761u) >> 21;
    uint8_t g;
    if (g_need_eip[h] == eip) return g_need_w[h];
    g = flags_needed(eip, TRAP, 2, &budget);
    if (eip && !page_writable(eip)) { g_need_eip[h] = eip; g_need_w[h] = g; }
    return g;
}

/* ---------- Operands ---------- */

/* address of a memory operand as a register pair for the X form (ra + rb, ra = 0 means 0) */
static void ea_pair(const Insn *b, int with_seg, int *ra, int *rb)
{
    int32_t d = (int32_t)b->disp;
    int base = b->base >= 0 ? R(b->base) : 0, idx = -1;
    if (b->index >= 0) {
        if (b->scale) { e(SLWI(12, R(b->index), b->scale)); idx = 12; } else idx = R(b->index);
    }
    if (with_seg && (b->seg == 0x64 || b->seg == 0x65)) {
        e(LWZ(11, b->seg == 0x65 ? OFF(gs_base) : OFF(fs_base), RCPU));
        if (!base) base = 11;
        else if (idx < 0) idx = 11;
        else { e(ADD(12, idx, 11)); idx = 12; }
    }
    /* base + index<<s + d: compute base+d alongside the shift (shorter chain up to the access) */
    if (d != 0 && d >= -32768 && d < 32768 && idx == 12 && base && base != 11) {
        e(ADDI(11, base, d));
        *ra = 11; *rb = 12;
        return;
    }
    if (d != 0 || (idx < 0 && !base)) {
        if (idx >= 0) addi_after(12, idx, d);
        else load_imm(12, (uint32_t)d);
        idx = 12;
    }
    if (idx < 0) { *ra = 0; *rb = base; return; }
    *ra = base;
    *rb = idx;
}

/* address without segment in register t (lea) */
static void ea_after(const Insn *b, int t)
{
    int32_t d = (int32_t)b->disp;
    int base = b->base >= 0 ? R(b->base) : -1, idx = b->index >= 0 ? R(b->index) : -1, s = b->scale;
    if (idx < 0) {
        if (base < 0) load_imm(t, (uint32_t)d);
        else addi_after(t, base, d);
        return;
    }
    if (base < 0) {
        if (s) { e(SLWI(t, idx, s)); addi_after(t, t, d); }
        else addi_after(t, idx, d);
        return;
    }
    if (!s && !d) { e(ADD(t, base, idx)); return; }
    if (s) {                            /* shift and base+d side by side, then one addition */
        e(SLWI(12, idx, s));
        if (d) { addi_after(11, base, d); e(ADD(t, 12, 11)); } else e(ADD(t, base, 12));
        return;
    }
    e(ADD(12, base, idx));
    addi_after(t, 12, d);
}

static void load_p(int t, int ra, int rb, int sz)
{
    if (sz == 4) e(LWBRX(t, ra, rb));
    else if (sz == 2) e(LHBRX(t, ra, rb));
    else e(LBZX(t, ra, rb));
}

static void save_p(int s, int ra, int rb, int sz)
{
    if (sz == 4 && g_sw_skip) { g_sw_skip = 0; g_sw_skipped = 1; return; }   /* dead store (sw_killer) */
    if (sz == 4) e(STWBRX(s, ra, rb));
    else if (sz == 2) e(STHBRX(s, ra, rb));
    else e(STBX(s, ra, rb));
}

/* byte access with base + 16-bit offset: D form without address computation */
static int byte_direct(const Insn *b)
{
    int32_t d = (int32_t)b->disp;
    return b->index < 0 && b->base >= 0 && !(b->seg == 0x64 || b->seg == 0x65) && d >= -32768 && d < 32768;
}

/* ---------- Stack slots in registers (loops) ----------
 * In loop blocks up to 4 frequently read 32-bit slots [base+d] (base = ebp or esp, unchanged in the block) are
 * held in r28..r31. Writes still go to memory (which is always current), reads come from the register. If an
 * instruction writes through another address, it checks whether it hits the slots (range from r13) and then
 * reloads them. A backward jump to the block start goes behind the preload.
 * Deferred write-back (g_sl_wb): if all other accesses of the loop provably run away from the stack (pointers
 * change by fixed steps per iteration, one check at the loop entry), the slots live only in the register in the
 * loop part and are written back only before each exit. */
#define SLOTS 4
#define RRANGE 2                      /* volatile: set again after every C call in the block (c_call) */
static int g_sl_active, g_sl_n, g_sl_base, g_sl_d[SLOTS], g_sl_lo, g_sl_len, g_sl_verify;
static const int g_sl_reg[SLOTS] = { 28, 29, 30, 31 };
static uint32_t *g_sl_slow[40];      /* check branch -> reload */
static int g_sl_nslow;
static unsigned long g_st_slots;
static int g_sl_wb;                     /* block with deferred write-back (in the loop part: g_sl_wb_now) */
static int g_sl_free_now;             /* current instruction provably writes below the slots (g_sl_free) */
static int g_sl_wmask;                 /* slots written in the loop part */
static int g_sl_last;                 /* last backward jump of the loop part */

/* written slots to memory (r12 as offset; CR and flags stay) */
static void sl_back_code(int msk)
{
    int k;
    for (k = 0; k < g_sl_n; k++)
        if (msk >> k & 1) {
            load_imm(12, (uint32_t)g_sl_d[k]);
            e(STWBRX(g_sl_reg[k], R(g_sl_base), 12));
        }
}

static inline int is_slot(const Insn *b, int base)
{
    return b->mem && b->base == base && b->index < 0 && !(b->seg == 0x64 || b->seg == 0x65);
}

static int slot_reg(const Insn *b, int sz)
{
    int k;
    if (!g_sl_active || sz != 4 || !is_slot(b, g_sl_base)) return -1;
    for (k = 0; k < g_sl_n; k++) if (g_sl_d[k] == (int32_t)b->disp) return g_sl_reg[k];
    return -1;
}

/* after every memory write: update the slot or check for overlap. ra < 0: address = base + d */
static int sw_store_note(const Insn *b, int val);

static void after_save(const Insn *b, int sz, int ra, int rb, int val)
{
    if (sz == 4 && val >= 0) {                          /* stack forwarding */
        if (!sw_store_note(b, val) && g_sw_skipped) {   /* left out, but no entry: store after all */
            g_sw_skipped = 0;
            if (ra < 0) { int t = val == 12 ? 11 : 12; load_imm(t, b->disp); ra = R(b->base); rb = t; }
            save_p(val, ra, rb, 4);
        }
    }
    g_sw_skip = g_sw_skipped = 0;
    if (!g_sl_active) return;
    if (is_slot(b, g_sl_base)) {
        int c = slot_reg(b, sz);
        if (c >= 0 && c != val) e(MR(c, val));
        return;
    }
    if (g_sl_wb_now) return;          /* checked at the loop entry: never hits the slots */
    if (g_sl_free_now) return;        /* stack below the slots (check at entry) */
    if (ra < 0) { addi_after(11, R(b->base), (int32_t)b->disp); e(SUBF(11, RRANGE, 11)); }
    else if (ra == 0) e(SUBF(11, RRANGE, rb));
    else { e(ADD(11, ra, rb)); e(SUBF(11, RRANGE, 11)); }
    e(CMPLWI(1, 11, g_sl_len));          /* cr1: CR0 can hold a merged condition */
    g_sl_slow[g_sl_nslow++] = p;
    e(BC_(BO_TRUE, 4 + CR_LT, 0));
}

/* range registers for the overlap check (if slots are active and checking is done) */
static void range_set(void)
{
    if (g_sl_active && g_sl_verify) addi_after(RRANGE, R(g_sl_base), g_sl_lo);
}

static void slots_load(void)
{
    int k;
    for (k = 0; k < g_sl_n; k++) {
        load_imm(12, (uint32_t)g_sl_d[k]);
        e(LWBRX(g_sl_reg[k], R(g_sl_base), 12));
    }
}

static int sw_load(const Insn *b, int t);
static int sw_copy_of(const Insn *b, int own);
static int sw_fwd_ok(const Insn *b);

/* address of a memory operand that the instruction reads and writes (or only writes): not needed if its store is
   left out (dead store) and its value comes from forwarding; then *ra = -1 (after_save computes it if it has to
   store after all) */
static void rmw_addr(const Insn *b, int *ra, int *rb)
{
    if (g_sw_skip && b->opsz == 4 && sw_fwd_ok(b)) { *ra = -1; *rb = 0; return; }
    ea_pair(b, 1, ra, rb);
}

/* value of a 32-bit memory operand that the instruction overwrites with a result computed from it: the copy register
   of the stack slot itself (the result goes there, sw_host keeps it as the slot's new copy) or r3 */
static int rmw_val(const Insn *b, int ra, int rb)
{
    int q = sw_copy_of(b, 1);
    if (q) return q;
    if (!sw_load(b, 3)) {
        if (ra < 0) { load_imm(12, b->disp); ra = R(b->base); rb = 12; }   /* (sw_fwd_ok promised forwarding) */
        load_p(3, ra, rb, 4);
    }
    return 3;
}

static int sw_load_narrow(const Insn *b, int sz, int t);

static void load_m(int t, const Insn *b, int sz)
{
    int ra, rb, c = slot_reg(b, sz);
    if (c >= 0) { if (c != t) e(MR(t, c)); return; }
    if (sz == 4 && sw_load(b, t)) return;   /* stack slot written or read in the block */
    if (sz < 4 && sw_load_narrow(b, sz, t)) return;     /* a byte or half of such a slot */
    if (sz == 1 && byte_direct(b)) { e(LBZ(t, (int32_t)b->disp, R(b->base))); return; }
    ea_pair(b, 1, &ra, &rb);
    load_p(t, ra, rb, sz);
}

static void save_m(int s, const Insn *b, int sz)
{
    int ra, rb;
    if (g_sl_wb_now) {                /* slot only in the register (write-back at the exit) */
        int c = slot_reg(b, sz);
        if (c >= 0) { if (c != s) e(MR(c, s)); return; }
    }
    if (sz == 1 && byte_direct(b)) {
        e(STB(s, (int32_t)b->disp, R(b->base)));
        after_save(b, sz, -1, 0, s);
        return;
    }
    if (sz == 4 && g_sw_skip) { ra = -1; rb = 0; }      /* left out (dead store): no address */
    else ea_pair(b, 1, &ra, &rb);
    save_p(s, ra, rb, sz);
    after_save(b, sz, ra, rb, s);
}

/* value of an Intel register of size sz (8/16 bit zero-extended) */
/* mov target,src (32 bit, registers) before a computation that overwrites target is not generated: the computation
   reads target from src (PowerPC has three operands). Applies only to the following instruction. */
static int g_mov_target = -1, g_mov_src = -1, g_mov_for = -1;
static unsigned long g_st_mov;

static int reg_val(int r, int sz, int t)
{
    if (sz == 4) return R(r == g_mov_target ? g_mov_src : r);
    if (sz == 2) { e(RLWINM(t, R(r), 0, 16, 31)); return t; }
    if (r < 4) e(RLWINM(t, R(r), 0, 24, 31)); else e(RLWINM(t, R(r - 4), 24, 24, 31));
    return t;
}

/* source register for a memory access of size sz (stb/sthbrx take only the low bits) */
static int reg_src(int r, int sz, int t)
{
    if (sz != 1 || r < 4) return R(r);
    e(RLWINM(t, R(r - 4), 24, 24, 31));
    return t;
}

static void reg_set(int r, int sz, int v)
{
    if (sz == 4) { if (R(r) != v) e(MR(R(r), v)); }
    else if (sz == 2) e(RLWIMI(R(r), v, 0, 16, 31));
    else if (r < 4) e(RLWIMI(R(r), v, 0, 24, 31));
    else e(RLWIMI(R(r - 4), v, 8, 16, 23));
}

static int rm_val(const Insn *b, int sz, int t)
{
    if (b->mem) {
        int c = slot_reg(b, sz);
        if (c >= 0) return c;
        if (sz == 4 && (c = sw_copy_of(b, 0)) > 0) return c;    /* read only: the copy register without mr */
        load_m(t, b, sz);
        return t;
    }
    return reg_val(b->rm, sz, t);
}

/* ---------- Flags in the translator ---------- */

/* art: CC_* (state in RE/RA/RB/RCIN), -1 (in memory), -2 (discarded: a merged compare, dead) */
static struct { int kind, sz, stored; } fs;

static void flags_put(void)
{
    if (fs.kind < 0 || fs.stored) return;
    e(LI(0, fs.kind)); e(STW(0, OFF(cc_op), RCPU));
    e(LI(0, fs.sz)); e(STW(0, OFF(cc_size), RCPU));
    e(STW(RE, OFF(cc_res), RCPU));
    e(STW(RA, OFF(cc_a), RCPU));
    e(STW(RB, OFF(cc_b), RCPU));
    e(STW(RCIN, OFF(cc_cin), RCPU));
    fs.stored = 1;
}

static void flags_new(int kind, int sz) { fs.kind = kind; fs.sz = sz; fs.stored = 0; }

typedef struct { int bit, truth; } Cond;     /* bit < 0: constant true/false */

static Cond cond_mk(int bit, int truth) { Cond r; r.bit = bit; r.truth = truth; return r; }

static uint32_t g_poison_eip;            /* debugging: eip of the instruction currently being generated */

static unsigned long g_st_cond_helper;

/* condition k is true if CR0 bit x is set (inverted: if it is not): result into CR0.EQ = not true */
static void cond_after_eq(int x, int reversed)
{
    if (reversed) e(CROR(CR_EQ, x, x)); else e(CRNOR(CR_EQ, x, x));
}

/* Flags are in memory, kind known only at run time: compute the common kinds (cmp/sub and test/logic, 8, 16 and 32 bit:
   the operands shifted to the top of the word, so that one compare holds for every size) directly from Cpu.cc_*,
   otherwise through C. Result: condition true <=> CR0.EQ = 0. */
static Cond cond_helper(int k)
{
    int kk = k >> 1, negated = k & 1;
    uint32_t *cont_sub = 0, *cont_sz = 0, *done1 = 0, *cont_logic = 0, *done2 = 0;
    g_st_cond_helper++;
    flags_put();
    xf_all_done();                       /* before the branches: all paths the same (the C path calls c_call) */
    if (kk != 0 && kk != 5 && !g_poison) {    /* overflow and parity only through C */
        e(LWZ(0, OFF(cc_op), RCPU));
        e(LWZ(11, OFF(cc_size), RCPU));
        e(CMPLWI(1, 11, 4)); cont_sz = p; e(BC_(BO_TRUE, 4 + CR_GT, 0));    /* sizes 1, 2, 4 (bzip2: cmpb) */
        e(RLWINM(11, 11, 3, 0, 31));
        e(SUBFIC(11, 11, 32));                /* shift: 24, 16, 0 */
        e(CMPWI(1, 0, CC_SUB)); cont_sub = p; e(BC_(BO_FALSE, 4 + CR_EQ, 0));
        e(LWZ(3, OFF(cc_a), RCPU)); e(LWZ(4, OFF(cc_b), RCPU));
        e(SLW(3, 3, 11)); e(SLW(4, 4, 11));
        switch (kk) {                       /* d - s: a = d, b = s */
        case 1: e(CMPLW(0, 3, 4)); cond_after_eq(CR_LT, negated); break;           /* b: a < b unsigned */
        case 2: e(CMPLW(0, 3, 4)); cond_after_eq(CR_EQ, negated); break;           /* e */
        case 3: e(CMPLW(0, 3, 4)); cond_after_eq(CR_GT, !negated); break;          /* be: not greater */
        case 4: e(LWZ(3, OFF(cc_res), RCPU)); e(SLW(3, 3, 11)); e(CMPWI(0, 3, 0)); cond_after_eq(CR_LT, negated); break;   /* s */
        case 6: e(CMPW(0, 3, 4)); cond_after_eq(CR_LT, negated); break;            /* l */
        default: e(CMPW(0, 3, 4)); cond_after_eq(CR_GT, !negated); break;          /* le */
        }
        done1 = p; e(B_(0));
        bc_set(cont_sub, p);
        e(CMPWI(1, 0, CC_LOGIC)); cont_logic = p; e(BC_(BO_FALSE, 4 + CR_EQ, 0));
        e(LWZ(3, OFF(cc_res), RCPU));
        e(SLW(3, 3, 11));
        e(CMPWI(0, 3, 0));
        switch (kk) {                       /* CF = OF = 0 */
        case 1: e(negated ? CRXOR(CR_EQ, CR_EQ, CR_EQ) : CREQV(CR_EQ, CR_EQ, CR_EQ)); break;   /* b: never */
        case 2: case 3: cond_after_eq(CR_EQ, negated); break;                       /* e, be = ZF */
        case 4: case 6: cond_after_eq(CR_LT, negated); break;                       /* s, l = SF */
        default: cond_after_eq(CR_GT, !negated); break;                             /* le = ZF | SF */
        }
        done2 = p; e(B_(0));
        bc_set(cont_sz, p);
        bc_set(cont_logic, p);
    }
    if (g_vt_on) fprintf(stderr, "rosetta: condition %d via C at %08x (flag kind %d)\n", k, g_poison_eip, fs.kind);
    if (g_poison) { load_imm(3, g_poison_eip); e(STW(3, OFF(eip), RCPU)); }
    e(MR(3, RCPU));
    e(LI(4, k));
    c_call((void *)op_condition);
    e(CMPWI(0, 3, 0));
    if (done1) { *done1 = B_((uint32_t)(uintptr_t)p - (uint32_t)(uintptr_t)done1); }
    if (done2) { *done2 = B_((uint32_t)(uintptr_t)p - (uint32_t)(uintptr_t)done2); }
    return cond_mk(CR_EQ, 0);
}

/* sign of the result of size sz in CR0 (LT) */
static void sign_re(void)
{
    if (fs.sz == 4) e(CMPWI(0, RE, 0));
    else { e(fs.sz == 1 ? EXTSB(11, RE) : EXTSH(11, RE)); e(CMPWI(0, 11, 0)); }
}

static Cond cond(int k)
{
    int negated = k & 1, kk = k >> 1;
    if (fs.kind < 0) return cond_helper(k);
    switch (fs.kind) {
    case CC_SUB:
        switch (kk) {
        case 1: e(CMPLW(0, RA, RB)); return cond_mk(CR_LT, !negated);
        case 2: e(CMPLW(0, RA, RB)); return cond_mk(CR_EQ, !negated);
        case 3: e(CMPLW(0, RA, RB)); return cond_mk(CR_GT, negated);
        case 4: sign_re(); return cond_mk(CR_LT, !negated);
        case 6: case 7:
            if (fs.sz == 4) e(CMPW(0, RA, RB));
            else { e(fs.sz == 1 ? EXTSB(11, RA) : EXTSH(11, RA)); e(fs.sz == 1 ? EXTSB(12, RB) : EXTSH(12, RB)); e(CMPW(0, 11, 12)); }
            return kk == 6 ? cond_mk(CR_LT, !negated) : cond_mk(CR_GT, negated);
        }
        return cond_helper(k);
    case CC_LOGIC:
        switch (kk) {
        case 0: case 1: return cond_mk(-1, negated);                       /* OF = CF = 0 */
        case 2: case 3: sign_re(); return cond_mk(CR_EQ, !negated);
        case 4: case 6: sign_re(); return cond_mk(CR_LT, !negated);
        case 7: sign_re(); return cond_mk(CR_GT, negated);
        }
        return cond_helper(k);
    case CC_ADD:
        if (kk == 1 && fs.sz == 4) { e(CMPLW(0, RE, RA)); return cond_mk(CR_LT, !negated); }
        /* fall through */
    case CC_INC: case CC_DEC: case CC_SHL: case CC_SHR: case CC_SAR:
        if (kk == 2) { sign_re(); return cond_mk(CR_EQ, !negated); }
        if (kk == 4) { sign_re(); return cond_mk(CR_LT, !negated); }
        return cond_helper(k);
    case CC_IMUL: case CC_MUL:
        if (kk == 0 || kk == 1) { e(CMPWI(0, RB, 0)); return cond_mk(CR_EQ, negated); }
        return cond_helper(k);
    }
    return cond_helper(k);
}

/* condition as 0/1 in register t */
static void cond_val(Cond b, int t)
{
    if (b.bit < 0) { e(LI(t, b.truth)); return; }
    e(LI(t, 1));
    e(BC_(b.truth ? BO_TRUE : BO_FALSE, b.bit, 8));
    e(LI(t, 0));
}

/* the same without a branch (mfcr): faster for random conditions, the branch would often be mispredicted
   (tools/setcc.c) */
static void cond_val_cr(Cond b, int t)
{
    if (b.bit < 0) { e(LI(t, b.truth)); return; }
    e(MFCR(t));
    e(RLWINM(t, t, b.bit + 1, 31, 31));
    if (!b.truth) e(XORI(t, t, 1));
}

/* condition prepared by the merged compare (otherwise from the flag state) */
/* conditions prepared by the merged compare, in order for the following readers */
static Cond g_ahead[8];
static int g_ahead_n, g_ahead_i;
static int g_ahead_data;               /* the merged compare reads memory: condition rather random */

static Cond cond_fetch(int k)
{
    if (g_ahead_i < g_ahead_n) return g_ahead[g_ahead_i++];
    return cond(k);
}

/* ---------- Exits ---------- */

/* Exit stub to a fixed target: becomes "b target" at the first run. Fourth word: the conditional jump that
   leads here (or 0), so that it can point straight at the target when chaining. */
static void stubby(uint32_t target, uint32_t *bc)
{
    if (g_sl_wb_now) sl_back_code(g_sl_wmask);
    e(LIS(3, target >> 16));
    e(ORI(3, 3, target & 0xffff));
    e(BL_((uint32_t)(uintptr_t)link_near(p) - (uint32_t)(uintptr_t)p));
    e((uint32_t)(uintptr_t)bc);
}

static void exit_kind(uint32_t eip, int kind)
{
    if (g_sl_wb_now) sl_back_code(g_sl_wmask);
    load_imm(3, eip);
    e(LI(4, kind));
    b_to(exit_near(p));
}

/* ---------- Return stack ----------
 * Ring buffer of (Intel return address, host address of the continuation), 64K entries, aligned to its size;
 * pointer in r13. call stores an entry and jumps with bl, ret compares and returns with blr: the G4 predicts
 * blr correctly through its link stack (bctr through the table it does not). */

/* store an entry (eip in register r, continuation = after the bl); returns the location of the bl (caller sets target) */
static uint32_t *shadow_call(int r)
{
    uint32_t *lis = p, *bl;
    e(LIS(5, 0)); e(ORI(5, 5, 0));
    e(STW(r, 0, RSHADOW));
    e(STW(5, 4, RSHADOW));
    e(ADDI(0, RSHADOW, 8));
    e(RLWIMI(RSHADOW, 0, 0, 13, 31));     /* advance in the ring */
    bl = p;
    e(BL_(0));
    lis[0] = LIS(5, (uint32_t)(uintptr_t)p >> 16);
    lis[1] = ORI(5, 5, (uint32_t)(uintptr_t)p & 0xffff);
    return bl;
}

/* ret: top entry matches the eip in r3 -> blr there; otherwise continue with the following code (table). At the
   start of the ring this reads the sentinel entry before it (0xffffffff, never matches): then the return goes
   through the table. */
static void shadow_back(void)
{
    e(LWZ(4, -8, RSHADOW));
    e(CMPLW(0, 4, 3));
    e(BC_(BO_FALSE, CR_EQ, 20));
    e(LWZ(5, -4, RSHADOW));
    e(ADDI(RSHADOW, RSHADOW, -8));
    e(MTLR(5));
    e(BLR);
}

/* indirect jump to the eip in r3 through the table */
/* Before it an inline cache with g_ic_ways entries: each new target is written into the next free entry at its
   first run (exit with kind = entry | 1), after that a hit jumps directly with b; when all are taken, it goes
   through the table. Several ways: changing targets (tables of function pointers, virtual calls) save the
   chain table -> mtctr -> bctr. */
static void indirect(void)
{
    uint32_t *ic[8], *fill;
    int k;
    for (k = 0; k < g_ic_ways; k++) {
        ic[k] = p;
        e(B_(0));                       /* becomes lis r4,eip>>16 */
        e(ORI(4, 4, 0));
        e(CMPLW(0, 3, 4));
        e(BC_(BO_FALSE, CR_EQ, 8));    /* no: next entry */
        e(B_(0));                       /* becomes b target */
    }
    e(RLWINM(11, 3, 3, 13, 28));        /* (eip & 0xffff) * 8 */
    e(LWZX(12, RTAB, 11));
    e(CMPLW(0, 12, 3));
    e(BC_(BO_FALSE, CR_EQ, 20));
    e(ADDI(11, 11, 4));
    e(LWZX(12, RTAB, 11));
    e(MTCTR(12));
    e(BCTR);
    e(LI(4, KIND_NORMAL));
    b_to(exit_near(p));
    for (k = 0; k < g_ic_ways; k++) {
        fill = p;
        load_imm(4, (uint32_t)(uintptr_t)ic[k] | 1);
        b_to(exit_near(p));
        ic[k][0] = B_((uint32_t)(uintptr_t)fill - (uint32_t)(uintptr_t)ic[k]);
    }
}

static void push_reg(int v)
{
    e(ADDI(R(ESP), R(ESP), -4));
    e(STWBRX(v, 0, R(ESP)));
}

/* ---------- Arithmetic instructions ---------- */

static int g_res;                       /* register with the result of the last arithmetic instruction */
static uint8_t g_afterwards_now;           /* flags that are still read after the instruction being generated */

/* alu: 0 add 1 or 2 adc 3 sbb 4 and 5 sub 6 xor 7 cmp, plus test (8). source: register with the value or -1 + imm.
   adc/sbb: RCIN must already hold the carry. */
static void alu_calc(int op, int sz, int vd, int vs, int has_imm, uint32_t imm, int vr, int needs)
{
    int kind = op == 0 ? CC_ADD : op == 2 ? CC_ADC : op == 3 ? CC_SBB : (op == 5 || op == 7) ? CC_SUB : CC_LOGIC;
    int only_flags = op == 7 || op == 8;
    int32_t si = (int32_t)imm;
    if (sz != 4 && has_imm) { imm &= sz == 1 ? 0xff : 0xffff; si = (int32_t)imm; }
    if (only_flags && !needs) return;
    if (has_imm && (needs || op == 2 || op == 3 || sz != 4 || !(
            ((op == 0 || op == 5 || op == 7) && si > -32768 && si < 32768) ||
            ((op == 1 || op == 4 || op == 6) && imm < 0x10000)))) {
        vs = imm_reg(imm, 4);
        has_imm = 0;
    }
    if (needs && (kind == CC_ADD || kind == CC_ADC || kind == CC_SUB || kind == CC_SBB)) {
        e(MR(RA, vd));
        e(MR(RB, vs));
    }
    if (only_flags) vr = RE;
    switch (op) {
    case 0: if (has_imm) e(ADDI(vr, vd, si)); else e(ADD(vr, vd, vs)); break;
    case 1: if (has_imm) e(ORI(vr, vd, imm)); else e(OR(vr, vd, vs)); break;
    case 2: e(ADD(0, vd, vs)); e(ADD(vr, 0, RCIN)); break;
    case 3: e(SUBF(0, vs, vd)); e(SUBF(vr, RCIN, 0)); break;
    case 4: case 8: if (has_imm) e(ANDI_(vr, vd, imm)); else e(AND(vr, vd, vs)); break;
    case 5: case 7: if (has_imm) e(ADDI(vr, vd, -si)); else e(SUBF(vr, vs, vd)); break;
    case 6: if (has_imm) e(XORI(vr, vd, imm)); else e(XOR(vr, vd, vs)); break;
    }
    if (sz != 4 && (op == 0 || op == 2 || op == 3 || op == 5 || op == 7)) e(RLWINM(vr, vr, 0, sz == 1 ? 24 : 16, 31));
    if (needs) {
        if (vr != RE) e(MR(RE, vr));
        flags_new(kind, sz);
    }
}

/* Fetch the operands of an ALU instruction. direction: 0 r/m op= reg, 1 reg op= r/m, 2 r/m op= imm, 3 eax op= imm.
   Memory destination: address stays in ra/rb, value in r3 (or in the copy register of a stack slot: w). */
typedef struct { int vd, vs, target, ra, rb, c, w; } AluOp;    /* c: stack slot register or -1; w: copy register
                                                                   computed in place or -1 */

static AluOp alu_operands(const Insn *b, int sz, int dir, int writes)
{
    AluOp o;
    o.vs = -1; o.target = -1; o.ra = 0; o.rb = 0; o.c = -1; o.w = -1;
    if (dir == 0) o.vs = reg_val(b->reg, sz, 4);
    else if (dir == 1) o.vs = rm_val(b, sz, 4);
    if (dir == 1) { o.target = b->reg; o.vd = reg_val(o.target, sz, 3); }
    else if (dir == 3) { o.target = EAX; o.vd = reg_val(EAX, sz, 3); }
    else if (!b->mem) { o.target = b->rm; o.vd = reg_val(o.target, sz, 3); }
    else if ((o.c = slot_reg(b, sz)) >= 0) { o.vd = o.c; if (writes && !g_sl_wb_now) ea_pair(b, 1, &o.ra, &o.rb); }
    else {
        if (sz == 4 && writes) {
            rmw_addr(b, &o.ra, &o.rb);
            o.vd = rmw_val(b, o.ra, o.rb);
            if (o.vd != 3) o.w = o.vd;
        } else if (sz == 4 && sw_fwd_ok(b)) {                    /* cmp/test of a forwarded slot: no address */
            if ((o.vd = sw_copy_of(b, 0)) <= 0) { sw_load(b, 3); o.vd = 3; }
        } else {
            ea_pair(b, 1, &o.ra, &o.rb);
            if (sz == 4 ? !sw_load(b, 3) : !sw_load_narrow(b, sz, 3)) load_p(3, o.ra, o.rb, sz);
            o.vd = 3;
        }
    }
    return o;
}

/* result register: the Intel register itself, the stack slot register, the slot's copy register or r5 */
static int alu_vr(const AluOp *o, int sz)
{
    if (o->target >= 0 && sz == 4) return R(o->target);
    if (o->w >= 0) return o->w;
    return o->c >= 0 ? o->c : 5;
}

static void alu_back(const Insn *b, const AluOp *o, int sz, int vr)
{
    if (o->target >= 0) { if (sz != 4) reg_set(o->target, sz, vr); return; }
    if (o->c >= 0 && g_sl_wb_now) return;     /* result is in the slot register */
    save_p(vr, o->ra, o->rb, sz);
    after_save(b, sz, o->ra, o->rb, vr);
}

/* x = mov r32,r32 and y overwrites its destination with a computation that reads it first (registers only, 32 bit):
   then the mov is dropped (*target, *src set) */
static int mov_together(const Insn *x, const Insn *y, int *target, int *src)
{
    int op = y->op, z;
    if ((x->op != 0x89 && x->op != 0x8b) || x->mem || x->opsz != 4 || x->seg) return 0;
    *target = x->op == 0x89 ? x->rm : x->reg;
    *src = x->op == 0x89 ? x->reg : x->rm;
    z = *target;
    if (z == *src || y->mem || y->opsz != 4 || y->seg || y->lock) return 0;
    if (op < 0x40 && (op & 7) < 6 && (op & 1) && (op >> 3) != 7) {   /* add/or/adc/sbb/and/sub/xor, 32 bit */
        if ((op & 7) < 2) return y->rm == z;
        if ((op & 7) < 4) return y->reg == z;
        return z == EAX;
    }
    if ((op == 0x81 || op == 0x83) && y->reg != 7) return y->rm == z;
    if (op == 0xc1) return y->rm == z && (y->reg == 4 || y->reg == 5 || y->reg == 7) && (y->imm & 31);
    if (op == 0xd1) return y->rm == z && (y->reg == 4 || y->reg == 5 || y->reg == 7);
    if (op == 0x40 + z || op == 0x48 + z) return 1;
    return 0;
}

static void gen_alu(const Insn *b, int op, int sz, int dir, int needs)
{
    AluOp o;
    int vr;
    if (op == 2 || op == 3) cond_val(cond(2), RCIN);     /* carry before, ahead of everything else (helper call) */
    o = alu_operands(b, sz, dir, op != 7 && op != 8);
    vr = alu_vr(&o, sz);
    alu_calc(op, sz, o.vd, o.vs, dir >= 2, b->imm, vr, needs);
    g_res = vr;
    if (op != 7 && op != 8) alu_back(b, &o, sz, vr);
}

/* mask m as rlwinm bounds (one run of ones, also wrapping around): 1 and *mb, *me set */
static int rlw_bounds(uint32_t m, int *mb, int *me)
{
    int i, starts = 0;
    if (m == 0 || m == 0xffffffffu) return 0;
    for (i = 0; i < 32; i++) {
        int cur = (m >> (31 - i)) & 1, prev = (m >> (31 - ((i + 31) & 31))) & 1, next = (m >> (31 - ((i + 1) & 31))) & 1;
        if (cur && !prev) { *mb = i; starts++; }
        if (cur && !next) *me = i;
    }
    return starts == 1;
}

/* or/and/xor of an 8- or 16-bit register with a constant whose flags nobody reads: on the whole host register
   (orb $0xff,%dl: one ori instead of extracting the byte, computing and inserting it again) */
static int alu_narrow_imm(const Insn *b, int op, int sz, int dir)
{
    int r, h, sh = 0, mb = 0, me = 0;
    uint32_t lane = sz == 1 ? 0xff : 0xffff, imm, clear;
    if ((sz != 1 && sz != 2) || (op != 1 && op != 4 && op != 6) || (dir != 2 && dir != 3) || (dir == 2 && b->mem))
        return 0;
    r = dir == 3 ? EAX : b->rm;
    if (sz == 1 && r >= 4) { h = R(r - 4); sh = 8; } else h = R(r);
    imm = b->imm & lane;
    if (op == 1) { if (imm) e(ORI(h, h, imm << sh)); return 1; }
    if (op == 6) { if (imm) e(XORI(h, h, imm << sh)); return 1; }
    clear = (~imm & lane) << sh;
    if (!clear) return 1;
    if (rlw_bounds(~clear, &mb, &me)) e(RLWINM(h, h, 0, mb, me));
    else { load_imm(0, clear); e(ANDC(h, h, 0)); }
    return 1;
}

static void gen_inc_dec(const Insn *b, int reg, int dec, int needs, int needs_cf)
{
    int ra = 0, rb = 0, vd, q, only_reg = 0;
    if (needs && needs_cf) cond_val(cond(2), RCIN);
    if (reg >= 0 && b->opsz == 2) {         /* 16 bit in a register (inc/dec cx): result zero-extended in r5 */
        e(RLWINM(5, R(reg), 0, 16, 31));
        e(ADDI(5, 5, dec ? -1 : 1));
        e(RLWIMI(R(reg), 5, 0, 16, 31));
        e(RLWINM(5, 5, 0, 16, 31));
        g_res = 5;
        if (needs) { e(MR(RE, 5)); flags_new(dec ? CC_DEC : CC_INC, 2); }
        return;
    }
    if (b->op == 0xfe) {                    /* 8 bit (incb/decb): result zero-extended in r5 */
        if (!b->mem) { e(ADDI(5, reg_val(b->rm, 1, 5), dec ? -1 : 1)); reg_set(b->rm, 1, 5); }
        else {
            ea_pair(b, 1, &ra, &rb);
            e(LBZX(5, ra, rb));
            e(ADDI(5, 5, dec ? -1 : 1));
            e(STBX(5, ra, rb));
            after_save(b, 1, ra, rb, -1);
        }
        e(RLWINM(5, 5, 0, 24, 31));
        g_res = 5;
        if (needs) { e(MR(RE, 5)); flags_new(dec ? CC_DEC : CC_INC, 1); }
        return;
    }
    if (reg < 0 && b->opsz == 2) {          /* 16 bit in memory (incw: gzip's frequency counters): result in r5 */
        ea_pair(b, 1, &ra, &rb);
        e(LHBRX(5, ra, rb));
        e(ADDI(5, 5, dec ? -1 : 1));
        e(STHBRX(5, ra, rb));
        after_save(b, 2, ra, rb, -1);
        e(RLWINM(5, 5, 0, 16, 31));
        g_res = 5;
        if (needs) { e(MR(RE, 5)); flags_new(dec ? CC_DEC : CC_INC, 2); }
        return;
    }
    if (reg >= 0) { vd = R(reg); q = reg_val(reg, 4, 0); }
    else {
        int c = slot_reg(b, 4);
        only_reg = c >= 0 && g_sl_wb_now;
        if (!only_reg) rmw_addr(b, &ra, &rb);
        vd = c >= 0 ? c : rmw_val(b, ra, rb);
        q = vd;
    }
    e(ADDI(vd, q, dec ? -1 : 1));
    g_res = vd;
    if (reg < 0 && !only_reg) { save_p(vd, ra, rb, 4); after_save(b, 4, ra, rb, vd); }
    if (needs) { e(MR(RE, vd)); flags_new(dec ? CC_DEC : CC_INC, 4); }
}

/* rol/ror by a fixed count (flags dead afterwards: otherwise the interpreter writes CF/OF) */
static void gen_rotate(const Insn *b, int right, int count)
{
    int ra = 0, rb = 0, vd, only_reg = 0, sh = right ? 32 - count : count;
    if (!b->mem) { e(RLWINM(R(b->rm), R(b->rm), sh, 0, 31)); return; }
    {
        int c = slot_reg(b, 4);
        only_reg = c >= 0 && g_sl_wb_now;
        if (!only_reg) rmw_addr(b, &ra, &rb);
        vd = c >= 0 ? c : rmw_val(b, ra, rb);
    }
    e(RLWINM(vd, vd, sh, 0, 31));
    if (!only_reg) { save_p(vd, ra, rb, 4); after_save(b, 4, ra, rb, vd); }
}

/* rol/ror r/m32 by cl (CF and OF dead; count 0 leaves the value) */
static void gen_rotate_cl(const Insn *b, int right)
{
    int ra = 0, rb = 0, vd, only_reg = 0;
    e(RLWINM(5, R(ECX), 0, 27, 31));
    if (right) e(SUBFIC(5, 5, 32));         /* rotating by 32 is rotating by 0 (rlwnm takes 5 bits) */
    if (!b->mem) { e(RLWNM(R(b->rm), R(b->rm), 5, 0, 31)); return; }
    {
        int c = slot_reg(b, 4);
        only_reg = c >= 0 && g_sl_wb_now;
        if (!only_reg) rmw_addr(b, &ra, &rb);
        vd = c >= 0 ? c : rmw_val(b, ra, rb);
    }
    e(RLWNM(vd, vd, 5, 0, 31));
    if (!only_reg) { save_p(vd, ra, rb, 4); after_save(b, 4, ra, rb, vd); }
}

/* rolw/rorw $n of a 16-bit register (rolw $8: the byte swap of ntohs): the half twice side by side, rotate */
static void gen_rotate16(const Insn *b, int right, int n)
{
    int r = R(b->rm);
    n &= 15;
    if (!n) return;
    e(RLWINM(0, r, 0, 16, 31));
    e(RLWIMI(0, 0, 16, 0, 15));
    e(RLWINM(0, 0, right ? 16 - n : n, 16, 31));
    e(RLWIMI(r, 0, 0, 16, 31));
}

/* shl/shr/sar/rol/ror of a byte register by 1..7 (hex digits: shrb $4); rotations only with CF and OF dead */
static void gen_shift8(const Insn *b, int kind, int n, int needs)
{
    int ra = 0, rb = 0, x;
    if (b->mem) {                           /* a byte in memory (shrb of flag bytes: gzip) */
        ea_pair(b, 1, &ra, &rb);
        e(LBZX(4, ra, rb));
        x = 4;
    } else x = reg_val(b->rm, 1, 4);        /* zero-extended byte */
    if (kind <= 1) {
        e(RLWIMI(x, x, 8, 16, 23));          /* the byte twice side by side */
        e(RLWINM(0, x, kind == 0 ? 24 + n : 32 - n, 24, 31));
        reg_set(b->rm, 1, 0);
        return;
    }
    if (needs) { e(MR(RA, x)); e(LI(RB, n)); }
    if (kind == 4) e(RLWINM(0, x, n, 24, 31));
    else if (kind == 5) e(RLWINM(0, x, 32 - n, 24, 31));
    else { e(EXTSB(0, x)); e(SRAWI(0, 0, n)); e(RLWINM(0, 0, 0, 24, 31)); }
    if (b->mem) { e(STBX(0, ra, rb)); after_save(b, 1, ra, rb, -1); }
    else reg_set(b->rm, 1, 0);
    if (needs) { e(MR(RE, 0)); flags_new(kind == 4 ? CC_SHL : kind == 5 ? CC_SHR : CC_SAR, 1); }
}

static void gen_shift(const Insn *b, int kind, int count, int needs)
{
    int ra = 0, rb = 0, vd, vr, only_reg = 0;
    if (b->opsz == 2 && !b->mem && count >= 0) {  /* 16 bit in a register (fixed count 1..31) */
        int rr = R(b->rm);
        if (kind == 7) e(EXTSH(5, rr)); else e(RLWINM(5, rr, 0, 16, 31));
        if (needs) { e(MR(RA, 5)); e(LI(RB, count)); }
        if (kind == 4) e(SLWI(0, 5, count)); else if (kind == 5) e(SRWI(0, 5, count)); else e(SRAWI(0, 5, count));
        e(RLWIMI(rr, 0, 0, 16, 31));
        if (needs) { e(RLWINM(RE, 0, 0, 16, 31)); flags_new(kind == 4 ? CC_SHL : kind == 5 ? CC_SHR : CC_SAR, 2); }
        return;
    }
    if (b->opsz == 2 && b->mem && count >= 0) {   /* a word in memory (fixed count 1..31) */
        ea_pair(b, 1, &ra, &rb);
        e(LHBRX(5, ra, rb));
        if (kind == 7) e(EXTSH(5, 5));
        if (needs) { e(MR(RA, 5)); e(LI(RB, count)); }
        if (kind == 4) e(SLWI(0, 5, count)); else if (kind == 5) e(SRWI(0, 5, count)); else e(SRAWI(0, 5, count));
        e(STHBRX(0, ra, rb));
        after_save(b, 2, ra, rb, -1);
        if (needs) { e(RLWINM(RE, 0, 0, 16, 31)); flags_new(kind == 4 ? CC_SHL : kind == 5 ? CC_SHR : CC_SAR, 2); }
        return;
    }
    if (!b->mem) { vd = reg_val(b->rm, 4, 0); vr = R(b->rm); }
    else {
        int c = slot_reg(b, 4);
        only_reg = c >= 0 && g_sl_wb_now;
        if (!only_reg) rmw_addr(b, &ra, &rb);
        vd = c >= 0 ? c : rmw_val(b, ra, rb);
        vr = vd;
    }
    if (count >= 0) {                      /* fixed count 1..31 */
        if (needs) { e(MR(RA, vd)); e(LI(RB, count)); }
        if (kind == 4) e(SLWI(vr, vd, count));
        else if (kind == 5) e(SRWI(vr, vd, count));
        else e(SRAWI(vr, vd, count));
        if (needs) { e(MR(RE, vr)); flags_new(kind == 4 ? CC_SHL : kind == 5 ? CC_SHR : CC_SAR, 4); }
    } else {                                /* count in cl; at 0 the flags stay unchanged */
        if (needs) { flags_put(); e(MR(5, vd)); }     /* before r0: flags_put uses r0 */
        e(RLWINM(0, R(ECX), 0, 27, 31));
        if (kind == 4) e(SLW(vr, vd, 0)); else if (kind == 5) e(SRW(vr, vd, 0)); else e(SRAW(vr, vd, 0));
        if (needs) {
            e(CMPWI(0, 0, 0));
            e(BC_(BO_TRUE, CR_EQ, 4 * 13));
            e(LI(6, kind == 4 ? CC_SHL : kind == 5 ? CC_SHR : CC_SAR)); e(STW(6, OFF(cc_op), RCPU));
            e(LI(6, 4)); e(STW(6, OFF(cc_size), RCPU));
            e(STW(vr, OFF(cc_res), RCPU)); e(STW(5, OFF(cc_a), RCPU)); e(STW(0, OFF(cc_b), RCPU));
            e(MR(RE, vr)); e(MR(RA, 5)); e(MR(RB, 0));
            e(NOP); e(NOP);
            fs.kind = -1;                    /* state is in memory (both paths) */
        }
    }
    if (b->mem && !only_reg) { save_p(vr, ra, rb, 4); after_save(b, 4, ra, rb, vr); }
}

/* shld/shrd r/m32, r32 by count 1..31 or (count < 0) by cl: gcc's 64-bit shifts (CoreFoundation reads big-endian
   numbers of binary property lists this way: 700,000 interpreted shld at Calculator's startup). Fixed count: two
   rotations; flags as shl/shr (CF the last bit shifted out of the destination, SF ZF PF of the result; OF of a
   one-bit shrd differs, the caller interprets then). With cl only without live flags (count 0 leaves them) */
static void gen_double_shift(const Insn *b, int left, int count, int needs)
{
    int ra = 0, rb = 0, vd, vr, only_reg = 0, src = reg_val(b->reg, 4, 0);
    if (!b->mem) { vd = reg_val(b->rm, 4, 0); vr = R(b->rm); }
    else {
        int c = slot_reg(b, 4);
        only_reg = c >= 0 && g_sl_wb_now;
        if (!only_reg) rmw_addr(b, &ra, &rb);
        vd = c >= 0 ? c : rmw_val(b, ra, rb);
        vr = vd;
    }
    if (count > 0) {
        if (needs) { e(MR(RA, vd)); e(LI(RB, count)); }
        if (left) { e(RLWINM(0, vd, count, 0, 31 - count)); e(RLWIMI(0, src, count, 32 - count, 31)); }
        else { e(RLWINM(0, vd, 32 - count, count, 31)); e(RLWIMI(0, src, 32 - count, 0, count - 1)); }
        e(MR(vr, 0));
        if (needs) { e(MR(RE, vr)); flags_new(left ? CC_SHL : CC_SHR, 4); }
    } else {                                /* count 0 gives the destination unchanged: shift by 32 is 0 on PowerPC */
        e(RLWINM(5, R(ECX), 0, 27, 31));
        e(SUBFIC(6, 5, 32));
        if (left) { e(SLW(0, vd, 5)); e(SRW(6, src, 6)); }
        else { e(SRW(0, vd, 5)); e(SLW(6, src, 6)); }
        e(OR(vr, 0, 6));
    }
    if (b->mem && !only_reg) { save_p(vr, ra, rb, 4); after_save(b, 4, ra, rb, vr); }
}

/* imul with overflow flag in RB */
static void gen_imul(int target, int x, int y, int has_imm, int32_t imm, int needs)
{
    if (has_imm) {
        if (imm >= -32768 && imm < 32768 && !needs) { e(MULLI(R(target), x, imm)); return; }
        y = imm_reg((uint32_t)imm, 5);
    }
    if (needs) {
        e(MULHW(6, x, y));
        e(MULLW(R(target), x, y));
        e(SRAWI(0, R(target), 31));
        e(XOR(6, 6, 0));
        e(CNTLZW(6, 6));
        e(SRWI(6, 6, 5));
        e(XORI(RB, 6, 1));
        e(MR(RE, R(target)));
        flags_new(CC_IMUL, 4);
    } else e(MULLW(R(target), x, y));
}

/* ---------- Merging compare and jump ---------- */

/* condition code of an instruction that reads flags (jcc, setcc, cmovcc), otherwise -1 */
static int consumer(const Insn *b)
{
    int op = b->op;
    if (op >= 0x70 && op <= 0x7f) return op & 15;
    if ((op >= (OP_0F | 0x80) && op <= (OP_0F | 0x8f)) || (op >= (OP_0F | 0x90) && op <= (OP_0F | 0x9f)) ||
        (op >= (OP_0F | 0x40) && op <= (OP_0F | 0x4f))) return op & 15;
    return -1;
}

/* CR0 from the result in v (size sz): condition 2/3 (zero), 4/6 (sign), 7 (zero or sign) */
static Cond res_cond(int v, int sz, int k)
{
    int kk = k >> 1, negated = k & 1;
    if (sz == 4) {
        /* convert the preceding instruction that wrote v into its CR0 form, otherwise compare */
        uint32_t w = p[-1];
        int op = (int)(w >> 26), xo = (int)((w >> 1) & 1023);
        if (op == 31 && (xo == 266 || xo == 40) && (int)((w >> 21) & 31) == v) p[-1] = w | 1;
        else if (op == 31 && (xo == 28 || xo == 444 || xo == 316) && (int)((w >> 16) & 31) == v) p[-1] = w | 1;
        else if (op == 14 && (int)((w >> 21) & 31) == v && ((w >> 16) & 31) != 0) p[-1] = (w & 0x03ffffffu) | 13u << 26;
        else if (!(op == 28 && (int)((w >> 16) & 31) == v)) e(CMPWI(0, v, 0));
    } else if (kk == 2 || kk == 3) e(CMPWI(0, v, 0));
    else e(RC_(sz == 1 ? EXTSB(0, v) : EXTSH(0, v)));
    if (kk == 2 || kk == 3) return cond_mk(CR_EQ, !negated);
    if (kk == 4 || kk == 6) return cond_mk(CR_LT, !negated);
    return cond_mk(CR_GT, negated);
}

/* cmp (and sub with conditions on the operands): compare before the computation */
static Cond cmp_fusion(const Insn *b, int alu, int sz, int dir, int k)
{
    int kk = k >> 1, negated = k & 1, has_imm = dir >= 2, sign = kk == 6 || kk == 7;
    uint32_t imm = b->imm;
    AluOp o = alu_operands(b, sz, dir, alu == 5);
    if (sz != 4) {
        imm &= sz == 1 ? 0xff : 0xffff;
        if (sign) {                         /* for 8/16 bit vd is in r3, vs in r4 */
            e(sz == 1 ? EXTSB(3, o.vd) : EXTSH(3, o.vd)); o.vd = 3;
            if (has_imm) imm = sz == 1 ? (uint32_t)(int32_t)(int8_t)imm : (uint32_t)(int32_t)(int16_t)imm;
            else { e(sz == 1 ? EXTSB(4, o.vs) : EXTSH(4, o.vs)); o.vs = 4; }
        }
    }
    if (kk == 4) {                          /* sign of d - s (cmp only) */
        if (has_imm) o.vs = imm_reg(imm, 4);
        e(SUBF(0, o.vs, o.vd));
        if (sz == 4) p[-1] |= 1; else e(RC_(sz == 1 ? EXTSB(0, 0) : EXTSH(0, 0)));
        return cond_mk(CR_LT, !negated);
    }
    if (has_imm) {
        int32_t si = (int32_t)imm;
        if ((sign || kk == 2) && si >= -32768 && si < 32768) e(CMPWI(0, o.vd, si));
        else if (!sign && imm < 0x10000) e(CMPLWI(0, o.vd, imm));
        else { o.vs = imm_reg(imm, 4); has_imm = 0; e(sign ? CMPW(0, o.vd, o.vs) : CMPLW(0, o.vd, o.vs)); }
    } else e(sign ? CMPW(0, o.vd, o.vs) : CMPLW(0, o.vd, o.vs));
    if (alu == 5) {
        int vr = alu_vr(&o, sz);
        alu_calc(5, sz, o.vd, o.vs, has_imm, imm, vr, 0);
        alu_back(b, &o, sz, vr);
    }
    switch (kk) {
    case 1: return cond_mk(CR_LT, !negated);
    case 2: return cond_mk(CR_EQ, !negated);
    case 3: return cond_mk(CR_GT, negated);
    case 6: return cond_mk(CR_LT, !negated);
    }
    return cond_mk(CR_GT, negated);            /* 7 */
}

static Cond test_fusion(const Insn *b, int sz, int dir, int k)
{
    int kk = k >> 1, negated = k & 1, vd, vs = -1;
    uint32_t imm = b->imm & (sz == 1 ? 0xffu : sz == 2 ? 0xffffu : 0xffffffffu);
    if (kk == 0 || kk == 1) return cond_mk(-1, negated);                   /* OF = CF = 0 */
    /* test a register against an immediate for zero: andi. on the full register, the mask cuts off the rest */
    if (dir >= 2 && (kk == 2 || kk == 3) && (dir == 3 || !b->mem) && (imm < 0x10000 || !(imm & 0xffff))) {
        int rr = dir == 3 ? EAX : b->rm;
        if (sz == 1 && rr >= 4) e(ANDI_(0, R(rr - 4), imm << 8));
        else if (imm >= 0x10000) e(ANDIS_(0, R(rr), imm >> 16));
        else e(ANDI_(0, R(rr), imm));
        return cond_mk(CR_EQ, !negated);
    }
    if (dir == 0) vs = reg_val(b->reg, sz, 4);
    vd = dir == 3 ? reg_val(EAX, sz, 3) : rm_val(b, sz, 3);
    if (dir >= 2) {
        if (imm < 0x10000) e(ANDI_(0, vd, imm));
        else if (!(imm & 0xffff)) e(ANDIS_(0, vd, imm >> 16));
        else e(RC_(AND(0, vd, imm_reg(imm, 4))));
    } else e(RC_(AND(0, vd, vs)));
    if (sz != 4 && kk != 2 && kk != 3) e(RC_(sz == 1 ? EXTSB(0, 0) : EXTSH(0, 0)));
    if (kk == 2 || kk == 3) return cond_mk(CR_EQ, !negated);
    if (kk == 4 || kk == 6) return cond_mk(CR_LT, !negated);
    return cond_mk(CR_GT, negated);
}

/* cmp or test (32 bit) for several readers: one compare in CR0, one condition per reader. 0: not possible. */
static int chains_fusion(const Insn *b, const int *ks, int nk, Cond *conds)
{
    int op = b->op, j, sign = 0, skip = 0, test = 0, dir = 2;
    if (b->opsz != 4) return 0;
    if (op == 0x39 || op == 0x3b || op == 0x3d) dir = op == 0x39 ? 0 : op == 0x3b ? 1 : 3;
    else if ((op == 0x81 || op == 0x83) && b->reg == 7) dir = 2;
    else if (op == 0x85) { test = 1; dir = 0; }
    else if (op == 0xa9) { test = 1; dir = 3; }
    else if (op == 0xf7 && b->reg == 0) test = 1;
    else return 0;
    for (j = 0; j < nk; j++) {
        int kk = ks[j] >> 1;
        if (test) { if (kk == 5) return 0; continue; }
        if (kk == 6 || kk == 7) sign = 1;
        else if (kk == 1 || kk == 3) skip = 1;
        else if (kk != 2) return 0;
    }
    if (sign && skip) return 0;
    if (test) {
        int vd, vs = -1;
        if (dir == 0) vs = R(b->reg);
        vd = dir == 3 ? R(EAX) : rm_val(b, 4, 3);
        if (dir >= 2) {
            if (b->imm < 0x10000) e(ANDI_(0, vd, b->imm));
            else if (!(b->imm & 0xffff)) e(ANDIS_(0, vd, b->imm >> 16));
            else e(RC_(AND(0, vd, imm_reg(b->imm, 4))));
        } else e(RC_(AND(0, vd, vs)));
    } else {
        AluOp o = alu_operands(b, 4, dir, 0);
        if (dir >= 2) {
            int32_t si = (int32_t)b->imm;
            if (sign && si >= -32768 && si < 32768) e(CMPWI(0, o.vd, si));
            else if (!sign && b->imm < 0x10000) e(CMPLWI(0, o.vd, b->imm));
            else { int r = imm_reg(b->imm, 4); e(sign ? CMPW(0, o.vd, r) : CMPLW(0, o.vd, r)); }
        } else e(sign ? CMPW(0, o.vd, o.vs) : CMPLW(0, o.vd, o.vs));
    }
    for (j = 0; j < nk; j++) {
        int kk = ks[j] >> 1, negated = ks[j] & 1;
        if (test) {
            if (kk <= 1) conds[j] = cond_mk(-1, negated);              /* OF = CF = 0 */
            else if (kk <= 3) conds[j] = cond_mk(CR_EQ, !negated);
            else if (kk == 7) conds[j] = cond_mk(CR_GT, negated);
            else conds[j] = cond_mk(CR_LT, !negated);                  /* 4, 6 */
        } else {
            if (kk == 2) conds[j] = cond_mk(CR_EQ, !negated);
            else if (kk == 1 || kk == 6) conds[j] = cond_mk(CR_LT, !negated);
            else conds[j] = cond_mk(CR_GT, negated);                   /* 3, 7 */
        }
    }
    return 1;
}

/* Generate a flag-setting instruction so that condition k is in CR0 afterwards. 0: not possible (nothing generated). */
static int fusion(const Insn *b, int k, Cond *r)
{
    int op = b->op, kk = k >> 1, negated = k & 1, alu, sz = 4, dir = 2;
    if (kk == 5) return 0;                  /* parity */
    if (op < 0x40 && (op & 7) < 6) { alu = op >> 3; sz = (op & 1) ? b->opsz : 1; dir = (op & 7) < 2 ? 0 : (op & 7) < 4 ? 1 : 3; }
    else if (op == 0x80 || op == 0x81 || op == 0x83) { alu = b->reg; sz = op == 0x80 ? 1 : b->opsz; }
    else if (op == 0x84 || op == 0x85) { alu = 8; sz = op == 0x84 ? 1 : b->opsz; dir = 0; }
    else if (op == 0xa8 || op == 0xa9) { alu = 8; sz = op == 0xa8 ? 1 : b->opsz; dir = 3; }
    else if (op == 0xf6 && b->reg == 0) { alu = 8; sz = 1; }
    else if (op == 0xf7 && b->reg == 0) { alu = 8; sz = 4; }
    else if ((op >= 0x40 && op <= 0x4f) || ((op == 0xff || op == 0xfe) && b->reg <= 1)) {
        if (kk != 2 && kk != 4) return 0;
        if (op >= 0xfe) gen_inc_dec(b, b->mem ? -1 : b->rm, b->reg, 0, 0);
        else gen_inc_dec(b, op & 7, op >= 0x48, 0, 0);
        *r = res_cond(g_res, op == 0xff ? 4 : op == 0xfe ? 1 : b->opsz, k);   /* 8/16 bit: g_res zero-extended */
        return 1;
    } else return 0;
    if (alu == 2 || alu == 3) return 0;
    if (alu == 7 || (alu == 5 && kk != 2 && kk != 4)) {
        if (kk == 0) return 0;
        *r = cmp_fusion(b, alu, sz, dir, k);
        return 1;
    }
    if (alu == 8) { *r = test_fusion(b, sz, dir, k); return 1; }
    if ((alu == 0 || alu == 5) && kk != 2 && kk != 4) return 0;
    gen_alu(b, alu, sz, dir, 0);
    *r = (kk == 0 || kk == 1) ? cond_mk(-1, negated) : res_cond(g_res, sz, k);
    return 1;
}

/* ---------- x87 native: stack in f24..f31 within the block ----------
 * Position p relative to TOP at block entry (st(i) at entry = position i); g_x7.d = net pushes since entry,
 * st(i) now = position i - d. Cpu.st_top keeps the entry value until the sync; only then are dirty live positions
 * written to Cpu.st and TOP and depth adjusted. Arithmetic is done in double precision, as in the
 * interpreter. */
#define X7F(k) (24 + (k))
typedef struct { signed char fpr; char dirty; } X7Slot;
typedef struct { X7Slot pl[16]; int d; uint8_t freemask; } X7State;     /* pl[p + 8], p = -8..7 */
static X7State g_x7;
static unsigned long g_st_x87;
static const double g_x87k[2] = { 1.0, 0.0 };

static void x7_empty(void)
{
    int k;
    for (k = 0; k < 16; k++) { g_x7.pl[k].fpr = -1; g_x7.pl[k].dirty = 0; }
    g_x7.d = 0;
    g_x7.freemask = 0xff;
}

/* r11 = RCPU + 8 * ((TOP at entry + pos) & 7): then lfd/stfd f, OFF(st)(r11) */
static void x7_addr(int pos)
{
    e(LWZ(11, OFF(st_top), RCPU));
    if (pos) e(ADDI(11, 11, pos));
    e(RLWINM(11, 11, 3, 26, 28));
    e(ADD(11, 11, RCPU));
}

/* sync to state z (code; z stays): write live dirty positions, then TOP and depth */
static void x7_back_code(const X7State *z)
{
    int pos, bis = 7 - z->d < 7 ? 7 - z->d : 7;
    for (pos = -z->d; pos <= bis; pos++) {
        const X7Slot *q = &z->pl[pos + 8];
        if (q->fpr >= 0 && q->dirty) { x7_addr(pos); e(STFD(X7F(q->fpr), OFF(st), 11)); }
    }
    if (z->d) {
        e(LWZ(11, OFF(st_top), RCPU));
        e(ADDI(11, 11, -z->d));
        e(RLWINM(11, 11, 0, 29, 31));
        e(STW(11, OFF(st_top), RCPU));
        e(LWZ(12, OFF(x87_depth), RCPU));
        e(ADDI(12, 12, z->d));
        e(CMPWI(1, 12, 8)); e(BC_(BO_FALSE, 4 + CR_GT, 8)); e(LI(12, 8));
        e(CMPWI(1, 12, 0)); e(BC_(BO_FALSE, 4 + CR_LT, 8)); e(LI(12, 0));
        e(STW(12, OFF(x87_depth), RCPU));
    }
}

static void x7_all_back(void) { x7_back_code(&g_x7); x7_empty(); }

/* free FPR; dead positions (outside the live range) give up theirs first */
static int x7_fpr_new(void)
{
    int k, pos;
    if (!g_x7.freemask)
        for (pos = -8; pos < 8; pos++) {
            X7Slot *q = &g_x7.pl[pos + 8];
            if (q->fpr >= 0 && (pos < -g_x7.d || pos > 7 - g_x7.d)) { g_x7.freemask |= (uint8_t)(1 << q->fpr); q->fpr = -1; }
        }
    for (k = 0; k < 8; k++) if (g_x7.freemask & (1 << k)) { g_x7.freemask &= (uint8_t)~(1 << k); return k; }
    return 0;                                   /* does not happen: at most 8 live positions */
}

/* FPR with st(i); 0 <= d <= 7 is guaranteed beforehand (see gen_x87) */
static int x7_fetch(int i)
{
    X7Slot *q = &g_x7.pl[i - g_x7.d + 8];
    if (q->fpr < 0) {
        q->fpr = (signed char)x7_fpr_new();
        q->dirty = 0;
        x7_addr(i - g_x7.d);
        e(LFD(X7F(q->fpr), OFF(st), 11));
    }
    return X7F(q->fpr);
}

static void x7_dirty(int i) { g_x7.pl[i - g_x7.d + 8].dirty = 1; }

/* new st(0); the position with the same physical register (pos + 8) is thereby dead */
static int x7_push(void)
{
    int pos;
    X7Slot *q;
    g_x7.d++;
    pos = -g_x7.d;
    if (pos + 8 <= 7) {
        X7Slot *prev = &g_x7.pl[pos + 16];
        if (prev->fpr >= 0) { g_x7.freemask |= (uint8_t)(1 << prev->fpr); prev->fpr = -1; prev->dirty = 0; }
    }
    q = &g_x7.pl[pos + 8];
    if (q->fpr < 0) q->fpr = (signed char)x7_fpr_new();
    q->dirty = 1;
    return X7F(q->fpr);
}

static void x7_pop(void)
{
    X7Slot *q = &g_x7.pl[-g_x7.d + 8];
    if (q->fpr >= 0) { g_x7.freemask |= (uint8_t)(1 << q->fpr); q->fpr = -1; }
    q->dirty = 0;
    g_x7.d--;
}

static void x7_exchange(int i)
{
    X7Slot *a, *b, t;
    x7_fetch(0);
    x7_fetch(i);
    a = &g_x7.pl[-g_x7.d + 8];
    b = &g_x7.pl[i - g_x7.d + 8];
    t = *a; *a = *b; *b = t;
    a->dirty = b->dirty = 1;
}

/* x87 instructions the translator does itself */
static int x87_can_full(const Insn *b)
{
    int op = b->op, r = b->reg, i = b->rm;
    if (b->mem)
        switch (op) {
        case 0xd8: case 0xdc: return r != 2 && r != 3;          /* fadd/fmul/fsub/fsubr/fdiv/fdivr m32/m64 */
        case 0xd9: case 0xdd: return r == 0 || r == 2 || r == 3; /* fld/fst/fstp m32/m64 */
        case 0xdb: return r <= 3;                                /* fild, fisttp, fist, fistp m32 */
        case 0xdf: return r == 0;                                /* fild m16 */
        default: return 0;
        }
    switch (op) {
    case 0xd8: case 0xdc: case 0xde: return r != 2 && r != 3;
    case 0xd9: return r == 0 || r == 1 || (r == 4 && i <= 1) || (r == 5 && (i == 0 || i == 6));
    case 0xdd: return r == 2 || r == 3;
    case 0xdb: case 0xdf: return r == 5 || r == 6;              /* fucomi/fcomi(p): merged with readers */
    }
    return 0;
}

static int is_x87_cmp(const Insn *b)
{
    return (b->op == 0xdb || b->op == 0xdf) && !b->mem && (b->reg == 5 || b->reg == 6);
}

static void x87_calc(int r, int z, int a, int b)     /* z = calc(r, a, b) as in the interpreter */
{
    switch (r) {
    case 0: e(FADD(z, a, b)); break;
    case 1: e(FMUL(z, a, b)); break;
    case 4: e(FSUB(z, a, b)); break;
    case 5: e(FSUB(z, b, a)); break;
    case 6: e(FDIV(z, a, b)); break;
    default: e(FDIV(z, b, a)); break;
    }
}

/* fisttp/fist/fistp m32: truncate, or round per the control word; outside int32 0x80000000 like Intel */
static void gen_fist(const Insn *b)
{
    int r = b->reg, s0 = x7_fetch(0), a;
    uint32_t *s1, *s2;
    xf_all_done();                           /* before the branches: all paths the same */
    /* every path puts the value that must not go beyond 2^31 into f0: rounded, or the source when truncating */
    if (r == 1) { e(FCTIWZ(1, s0)); e(FMR(0, s0)); }
    else {
        uint32_t *j_near, *j_from, *f1, *f2;
        e(LHZ(0, OFF(fpu_ctl), RCPU));
        e(RLWINM(0, 0, 22, 30, 31));            /* RC */
        e(CMPWI(1, 0, 0)); j_near = p; e(BC_(BO_TRUE, 4 + CR_EQ, 0));
        e(CMPWI(1, 0, 3)); j_from = p; e(BC_(BO_TRUE, 4 + CR_EQ, 0));
        e(FMR(1, s0)); e(MR(3, RCPU)); c_call((void *)x87_round); e(FMR(0, 1)); e(FCTIWZ(1, 1));
        f1 = p; e(B_(0));
        bc_set(j_near, p); e(FCTIW(1, s0)); e(FADD(0, s0, F_MAGIC)); e(FSUB(0, 0, F_MAGIC));
        f2 = p; e(B_(0));
        bc_set(j_from, p); e(FCTIWZ(1, s0)); e(FMR(0, s0));
        *f1 = B_((uint32_t)(uintptr_t)p - (uint32_t)(uintptr_t)f1);
        *f2 = B_((uint32_t)(uintptr_t)p - (uint32_t)(uintptr_t)f2);
    }
    e(STFD(1, TMP(1), 1));
    e(LWZ(3, TMP(1) + 4, 1));
    e(ADDIS(0, 3, 0x8000)); e(CMPWI(1, 0, -1));
    s1 = p; e(BC_(BO_FALSE, 4 + CR_EQ, 0));
    e(FCMPU(1, 0, F_2POW31));
    s2 = p; e(BC_(BO_TRUE, 4 + CR_LT, 0));
    e(LIS(3, 0x8000));
    bc_set(s1, p);
    bc_set(s2, p);
    a = ea_in(b);
    e(STWBRX(3, 0, a));
    after_save(b, 4, 0, a, 3);
    if (r != 2) x7_pop();
}

static void gen_x87(const Insn *b)
{
    int op = b->op, r = b->reg, i = b->rm, s0, si, f;
    g_st_x87++;
    if (g_x7.d < 0 || g_x7.d >= 8) x7_all_back();  /* all st(0..7) and one push in the range -8..7 */
    if (b->mem)
        switch (op) {
        case 0xd8: case 0xdc:
            m_load(0, b, op == 0xd8 ? 'F' : 'D');
            s0 = x7_fetch(0);
            x87_calc(r, s0, s0, 0);
            x7_dirty(0);
            return;
        case 0xd9: case 0xdd:
            if (r == 0) {
                int z;
                if (g_vl_cur) { f = x7_push(); vl_load(f, b); return; }   /* fld from an array (ring) */
                if (g_kaddr) {                   /* fld of a constant */
                    f = x7_push();
                    if (!pool_load(f, op == 0xd9 ? 'F' : 'D')) {
                        z = m_pre(b, op == 0xd9 ? 'F' : 'D');
                        e(op == 0xd9 ? LFS(f, z, RCPU) : LFD(f, z, RCPU));
                    }
                    return;
                }
                if ((z = fw_find(b, op == 0xd9 ? 4 : 8)) >= 0) { f = x7_push(); e(FMR(f, z)); return; }   /* stored in the block */
                z = m_pre(b, op == 0xd9 ? 'F' : 'D');
                f = x7_push();
                e(op == 0xd9 ? LFS(f, z, RCPU) : LFD(f, z, RCPU));
                return;
            }
            fw_write(b, op == 0xd9 ? 4 : 8);
            if (op == 0xdd && g_x7.pl[-g_x7.d + 8].fpr < 0) {  /* st(0) is only in Cpu.st: copy without the FPU */
                int a, q;
                x7_addr(-g_x7.d);
                e(LWZ(3, OFF(st), 11)); e(LWZ(4, OFF(st) + 4, 11));
                if ((q = fw_new(b, 8)) >= 0) { e(LFD(q, OFF(st), 11)); fw_shadow_write(q, 8); }   /* for later reads */
                a = ea_in(b);
                tmp_save(0, 'D', a);
                after_save(b, 8, 0, a, 3);
                if (r == 3) x7_pop();
                return;
            }
            s0 = x7_fetch(0);
            m_save(s0, b, op == 0xd9 ? 'F' : 'D');
            {
                int q = fw_new(b, op == 0xd9 ? 4 : 8);
                if (q >= 0) { e(op == 0xd9 ? FRSP(q, s0) : FMR(q, s0)); fw_shadow_write(q, op == 0xd9 ? 4 : 8); }
            }
            if (r == 3) x7_pop();
            return;
        case 0xdb: case 0xdf:
            if (r == 0) {                       /* fild m32 / m16 */
                int a = ea_in(b), z;
                if (op == 0xdb) e(LWBRX(3, 0, a)); else { e(LHBRX(3, 0, a)); e(EXTSH(3, 3)); }
                f = x7_push();
                z = tmp_new();
                e(XORIS(0, 3, 0x8000)); e(STW(0, z + 4, 1)); e(LIS(0, 0x4330)); e(STW(0, z, 1));
                e(LFD(0, z, 1)); e(FSUB(f, 0, F_MAGIC));
                return;
            }
            gen_fist(b);
            return;
        }
    switch (op) {
    case 0xd8:
        s0 = x7_fetch(0); si = x7_fetch(i);
        x87_calc(r, s0, s0, si);
        x7_dirty(0);
        return;
    case 0xdc: case 0xde: {                     /* st(i) = st(i) op st(0); sub/div swapped */
        int o = r == 4 ? 5 : r == 5 ? 4 : r == 6 ? 7 : r == 7 ? 6 : r;
        s0 = x7_fetch(0); si = x7_fetch(i);
        x87_calc(o, si, si, s0);
        x7_dirty(i);
        if (op == 0xde) x7_pop();
        return;
    }
    case 0xd9:
        if (r == 0) { si = x7_fetch(i); f = x7_push(); if (f != si) e(FMR(f, si)); return; }
        if (r == 1) { x7_exchange(i); return; }
        if (r == 4) { s0 = x7_fetch(0); e(i == 0 ? FNEG(s0, s0) : FABS(s0, s0)); x7_dirty(0); return; }
        f = x7_push();                          /* fld1, fldz */
        load_imm(12, (uint32_t)(uintptr_t)g_x87k);
        e(LFD(f, i == 0 ? 0 : 8, 12));
        return;
    case 0xdd:                                  /* fst/fstp st(i) */
        s0 = x7_fetch(0);
        if (i) { si = x7_fetch(i); e(FMR(si, s0)); x7_dirty(i); }
        if (r == 3) x7_pop();
        return;
    }
}

/* ---------- Interpreter in the middle of a block ----------
 * What the translator cannot do and does not change the control flow, the interpreter executes without leaving
 * the block: store registers, call step(), reload registers. The block then continues. */

static int inline_possible(const Insn *b)
{
    int op = b->op;
    if (op == 0x90 && b->rep) return 0;     /* pause (spin loop): to the dispatcher, which lets other threads run first */
    if ((op >= 0x70 && op <= 0x7f) || (op >= (OP_0F | 0x80) && op <= (OP_0F | 0x8f))) return 0;
    switch (op) {
    case 0x9a: case 0xc2: case 0xc3: case 0xca: case 0xcb: case 0xcc: case 0xcd: case 0xce: case 0xcf:
    case 0xe0: case 0xe1: case 0xe2: case 0xe3: case 0xe8: case 0xe9: case 0xea: case 0xeb: case 0xf4:
    case OP_0F | 0x05: case OP_0F | 0x0b: case OP_0F | 0x34: case OP_0F | 0x35:
        return 0;
    case 0xff:
        return b->reg < 2 || b->reg == 6;
    }
    return 1;
}

/* x87 and SSE that step() passes to sse_x87_step: call directly, with the instruction decoded at translation */
static int fp_direct_possible(const Insn *b)
{
    int op = b->op, o = op & 0xff;
    if (op >= 0xd8 && op <= 0xdf) return 1;
    if (op < OP_0F || op > (OP_0F | 0xff)) return 0;
    return (o >= 0x10 && o <= 0x17) || (o >= 0x28 && o <= 0x2f) || (o >= 0x50 && o <= 0x7f && o != 0x77) ||
           o == 0xc2 || (o >= 0xc4 && o <= 0xc6) || o >= 0xd0;
}

/* return 0: executed; otherwise the block returns to the dispatcher, step() does the instruction (or reports it) */
static int fp_direct(Cpu *c, const Insn *b)
{
    int r;
    if (g_vt_on) interp_tally(b->eip);
    xmm_intel(c);
    c->eip = b->eip;
    r = sse_x87_step(c, b);
    if (r != RUN_OK) {
        if (g_vt_on) fprintf(stderr, "rosetta: direct call back to step(): op %03x reg %d at %08x\n", b->op, b->reg, b->eip);
        return r ? r : 1;
    }
    c->insns++;
    return 0;
}

static void gen_fp_direct(const Insn *x, uint8_t l, uint8_t s)
{
    int op = x->op, o = op & 0xff, i, n = (int)((sizeof *x + 3) / 4), reads_gpr = -1, writes_gpr = -1;
    uint32_t *ovf, *data, *jump;
    if (op >= OP_0F) xf_all_forget();   /* SSE works on Cpu.xmm; x87 leaves the xmm registers alone */
    if (op < OP_0F || o == 0xae) x7_all_back();     /* x87 (and fxsave) works on Cpu.st */
    if (l || (s && s != TRAP)) flags_put();
    /* integer registers: address registers always; source/destination for the few instructions that read or write one */
    if (op >= OP_0F && !x->mem && (o == 0x6e || o == 0xc4 || o == 0x2a)) reads_gpr = x->rm;
    if (op == 0xdf && !x->mem && x->reg == 4) writes_gpr = EAX;                                /* fnstsw ax */
    if (op >= OP_0F && !x->mem && o == 0x7e && (x->p66 || !x->rep)) writes_gpr = x->rm;       /* movd r32 */
    if (op >= OP_0F && (o == 0x50 || o == 0xd7 || o == 0xc5 || o == 0x2c || o == 0x2d)) writes_gpr = x->reg;
    if (x->mem) {
        if (x->base >= 0) e(STW(R(x->base), OFF(r[x->base]), RCPU));
        if (x->index >= 0) e(STW(R(x->index), OFF(r[x->index]), RCPU));
    }
    if (reads_gpr >= 0) e(STW(R(reads_gpr), OFF(r[reads_gpr]), RCPU));
    if (writes_gpr >= 0) e(STW(R(writes_gpr), OFF(r[writes_gpr]), RCPU));   /* partial write (ax) */
    ovf = p;                              /* put the decoded instruction into the code as data */
    e(B_(0));
    data = p;
    for (i = 0; i < n; i++) e(0);
    memcpy(data, x, sizeof *x);
    *ovf = B_((uint32_t)(uintptr_t)p - (uint32_t)(uintptr_t)ovf);
    if (g_sl_wb_now) sl_back_code(g_sl_wmask);   /* C reads guest memory */
    e(MR(3, RCPU));
    load_imm(4, (uint32_t)(uintptr_t)data);
    c_call((void *)fp_direct);
    e(CMPWI(0, 3, 0));
    jump = p;
    e(0);
    for (i = 0; i < 8; i++) e(STW(R(i), OFF(r[i]), RCPU));   /* fallback: state complete for step() */
    exit_kind(x->eip, KIND_INTERPRET);
    *jump = BC_(BO_TRUE, CR_EQ, (uint32_t)(uintptr_t)p - (uint32_t)(uintptr_t)jump);
    if (writes_gpr >= 0) e(LWZ(R(writes_gpr), OFF(r[writes_gpr]), RCPU));
    if (g_sl_active && x->mem) slots_load();   /* the instruction may have written a slot */
    if (l || s) { fs.kind = -1; fs.stored = 1; }
    g_st_fp_direct++;
}

static void gen_interpret(const Insn *x, uint8_t l, uint8_t s)
{
    int i;
    uint32_t *jump;
    if (fp_direct_possible(x)) { gen_fp_direct(x, l, s); return; }
    xf_all_forget();                /* the interpreter reads and writes Cpu.xmm */
    x7_all_back();                  /* and Cpu.st */
    if (l || (s && s != TRAP)) flags_put();
    for (i = 0; i < 8; i++) e(STW(R(i), OFF(r[i]), RCPU));
    if (g_sl_wb_now) sl_back_code(g_sl_wmask);   /* the interpreter reads guest memory */
    load_imm(3, x->eip);
    e(STW(3, OFF(eip), RCPU));
    e(MR(3, RCPU));
    c_call((void *)g_step_fn);
    e(CMPWI(0, 3, 0));
    jump = p;
    e(0);
    exit_kind(x->eip, KIND_INTERPRET);   /* error: the dispatcher reports it */
    *jump = BC_(BO_TRUE, CR_EQ, (uint32_t)(uintptr_t)p - (uint32_t)(uintptr_t)jump);
    for (i = 0; i < 8; i++) e(LWZ(R(i), OFF(r[i]), RCPU));
    if (g_sl_active) slots_load();      /* the interpreter may have written slots */
    if (l || s) { fs.kind = -1; fs.stored = 1; }
}

/* mul, imul, div, idiv with 32 bit (f7 /4../7). div/idiv only if edx:eax fits in 32 bit and the divisor is not 0
   (or -1); otherwise the interpreter computes, including the exception. */
static void gen_muldiv(const Insn *b, int needs)
{
    int v = rm_val(b, 4, 4);
    if (b->reg == 4 || b->reg == 5) {
        e(b->reg == 4 ? MULHWU(5, R(EAX), v) : MULHW(5, R(EAX), v));
        e(MULLW(R(EAX), R(EAX), v));
        e(MR(R(EDX), 5));
        if (needs) {                      /* overflow (CF = OF) in RB as in the interpreter */
            if (b->reg == 4) { e(ADDIC(0, 5, -1)); e(SUBFE(RB, 0, 5)); }
            else { e(SRAWI(0, R(EAX), 31)); e(XOR(0, 0, 5)); e(ADDIC(6, 0, -1)); e(SUBFE(RB, 6, 0)); }
            e(MR(RE, R(EAX)));
            e(LI(RA, 0));
            flags_new(b->reg == 4 ? CC_MUL : CC_IMUL, 4);
        }
        return;
    }
    {
        uint32_t *slow[3], *done;
        int nl = 0, j;
        xf_all_forget();                /* both paths (fast, interpreter) with an empty cache */
        x7_all_back();
        if (b->reg == 6) e(CMPWI(1, R(EDX), 0));
        else { e(SRAWI(0, R(EAX), 31)); e(CMPW(1, R(EDX), 0)); }
        slow[nl++] = p; e(BC_(BO_FALSE, 4 + CR_EQ, 0));
        e(CMPWI(1, v, 0));
        slow[nl++] = p; e(BC_(BO_TRUE, 4 + CR_EQ, 0));
        if (b->reg == 7) { e(CMPWI(1, v, -1)); slow[nl++] = p; e(BC_(BO_TRUE, 4 + CR_EQ, 0)); }
        e(b->reg == 6 ? DIVWU(5, R(EAX), v) : DIVW(5, R(EAX), v));
        e(MULLW(6, 5, v));
        e(SUBF(R(EDX), 6, R(EAX)));
        e(MR(R(EAX), 5));
        done = p; e(B_(0));
        for (j = 0; j < nl; j++) bc_set(slow[j], p);
        gen_interpret(b, 0, 0);       /* div leaves the flags unchanged */
        *done = B_((uint32_t)(uintptr_t)p - (uint32_t)(uintptr_t)done);
    }
}

/* ---------- SSE2 with AltiVec ---------- */

static const uint8_t g_weights[16] __attribute__((aligned(16))) = { 1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128 };

/* gen_sse works on mm0..mm7: Cpu.mm, 16 bytes apart, the register in the first 8 (the other 8 are scratch: lane
   operations on them do no harm, only the first 8 are ever read as the register) */
static int g_mmx;
static int xmm_off(int n) { return g_mmx ? OFF(mm) + 16 * n : OFF(xmm) + 16 * n; }

/* address of the memory operand in a register (for lwz/stw with offset) */
static int ea_in(const Insn *b)
{
    int ra, rb;
    int32_t d = (int32_t)b->disp;
    if (b->base >= 0 && b->index < 0 && !(b->seg == 0x64 || b->seg == 0x65) && d >= -32768 && d < 32768) {
        if (!d) return R(b->base);         /* [base]: without computation */
        e(ADDI(12, R(b->base), d));        /* [base + d]: one addi instead of li + add */
        return 12;
    }
    ea_pair(b, 1, &ra, &rb);
    if (!ra) return rb;
    e(ADD(12, ra, rb));
    return 12;
}

static void xf_forget(int n);

/* Cpu.xmm holds the raw Intel bytes (lane 0 at the lowest address). Instructions that only move whole lanes
   (unpack, shuffle, byte shifts) and byte-wise ones work on that form directly. Arithmetic on 16- and 32-bit lanes and
   on floats works on the reversed form (all 16 bytes reversed, one vperm): each lane is then in host byte order, only
   the order of the lanes is reversed, which element-wise operations do not notice. */
static const uint8_t g_vrev[16] __attribute__((aligned(16))) = { 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };
static const uint8_t g_vrev8[16] __attribute__((aligned(16))) = { 7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8 };
static const uint8_t g_vmulh[16] __attribute__((aligned(16))) = { 0, 1, 16, 17, 4, 5, 20, 21, 8, 9, 24, 25, 12, 13, 28, 29 };
static const uint8_t g_v2p31[16] __attribute__((aligned(16))) = { 0x4f, 0, 0, 0, 0x4f, 0, 0, 0, 0x4f, 0, 0, 0, 0x4f, 0, 0, 0 };

/* vperm patterns of the shuffles (pshufd/hw/lw, shufps, punpck*qdq): built at translation time, each one once */
#define VPOOL 2048
static uint8_t g_vpool[VPOOL][16] __attribute__((aligned(16)));
static int g_vpool_n;

static void vconst(int v, const uint8_t *m)
{
    int k;
    if (((uintptr_t)m & 15) == 0 && (m == g_vrev || m == g_vrev8 || m == g_vmulh || m == g_v2p31)) k = -1;
    else {
        for (k = 0; k < g_vpool_n && memcmp(g_vpool[k], m, 16); k++) ;
        if (k == g_vpool_n) {
            if (k == VPOOL) { fprintf(stderr, "rosetta: vector constant pool full\n"); abort(); }
            memcpy(g_vpool[g_vpool_n++], m, 16);
        }
        m = g_vpool[k];
    }
    load_imm(12, (uint32_t)(uintptr_t)m);
    e(LVX(v, 0, 12));
}

static void vx_load(int v, int n) { e(LI(0, xmm_off(n))); e(LVX(v, RCPU, 0)); }
static void vx_store(int v, int n) { e(LI(0, xmm_off(n))); e(STVX(v, RCPU, 0)); }

/* second operand (xmm or aligned m128; mm or m64 at any alignment, into the first 8 bytes) into v; may use v2, v3 */
static void vsrc_load(int v, const Insn *b)
{
    int ra, rb;
    if (b->mem && g_mmx) {                  /* the two quadwords that hold the 8 bytes, lined up with lvsl */
        int a = ea_in(b);
        e(LVSL(2, 0, a));
        e(LVX(v, 0, a));
        e(ADDI(11, a, 7));
        e(LVX(3, 0, 11));
        e(VPERM(v, v, 3, 2));
        return;
    }
    if (b->mem) { ea_pair(b, 1, &ra, &rb); e(LVX(v, ra, rb)); }
    else vx_load(v, b->rm);
}

/* 64-bit lanes of xmm n shifted by a constant through r3/r4 (AltiVec has no 64-bit elements) */
static void gen_shift_q(int n, int c, int left)
{
    int k;
    for (k = 0; k < 2; k++) {
        int o = xmm_off(n) + 8 * k;
        e(LI(0, o)); e(LWBRX(3, RCPU, 0));              /* low word */
        e(LI(0, o + 4)); e(LWBRX(4, RCPU, 0));          /* high word */
        if (!left && c >= 32) { e(c > 32 ? SRWI(3, 4, c - 32) : MR(3, 4)); e(LI(4, 0)); }
        else if (!left) { e(SRWI(3, 3, c)); e(SLWI(5, 4, 32 - c)); e(OR(3, 3, 5)); e(SRWI(4, 4, c)); }
        else if (c >= 32) { e(SLWI(4, 3, c - 32)); e(LI(3, 0)); }
        else { e(SLWI(4, 4, c)); e(SRWI(5, 3, 32 - c)); e(OR(4, 4, 5)); e(SLWI(3, 3, c)); }
        e(LI(0, o)); e(STWBRX(3, RCPU, 0));
        e(LI(0, o + 4)); e(STWBRX(4, RCPU, 0));
    }
}

static void gen_sse_on(const Insn *b);
static unsigned long g_st_mmx;

static void gen_sse(const Insn *b)
{
    g_mmx = is_mmx(b);
    if (g_mmx) g_st_mmx++;
    gen_sse_on(b);
    g_mmx = 0;
}

/* MMX where the 64-bit form is not the low half of the 128-bit one; 0: as SSE2 */
static int gen_mmx_own(const Insn *b, int op, int d, int c)
{
    int a, i;
    uint8_t m[16];
    switch (op) {
    case 0x77: return 1;                    /* emms: the mm registers are apart from the x87 stack */
    case 0x6f:                              /* movq mm, mm/m64 */
        vsrc_load(0, b);
        vx_store(0, b->reg);
        return 1;
    case 0x7f: case 0xe7:                   /* movq mm/m64, mm; movntq: 8 bytes through r3/r4 (any alignment) */
        if (!b->mem) { vx_load(0, b->reg); vx_store(0, b->rm); return 1; }
        a = ea_in(b);
        e(LWZ(3, d, RCPU)); e(LWZ(4, d + 4, RCPU));
        e(STW(3, 0, a)); e(STW(4, 4, a));
        after_save(b, 8, 0, a, -1);
        return 1;
    case 0x6e:                              /* movd mm, r/m32: the high half becomes 0 */
        if (!b->mem) { e(LI(0, d)); e(STWBRX(R(b->rm), RCPU, 0)); }
        else if ((i = slot_reg(b, 4)) >= 0) { e(LI(0, d)); e(STWBRX(i, RCPU, 0)); }
        else { a = ea_in(b); e(LWZ(3, 0, a)); e(STW(3, d, RCPU)); }
        e(LI(3, 0)); e(STW(3, d + 4, RCPU));
        return 1;
    case 0xd7:                              /* pmovmskb r32, mm: the top bits of the 8 bytes */
        vx_load(0, b->rm);
        e(VSPLTISB(1, 0));
        e(VCMPGTSB(0, 1, 0));
        load_imm(12, (uint32_t)(uintptr_t)g_weights); e(LVX(2, 0, 12));
        e(VAND(0, 0, 2));
        e(VSUM4UBS(0, 0, 1));
        e(LI(0, OFF(sse_tmp))); e(STVX(0, RCPU, 0));
        e(LWZ(3, OFF(sse_tmp), RCPU)); e(LWZ(4, OFF(sse_tmp) + 4, RCPU));
        e(ADD(R(b->reg), 3, 4));
        return 1;
    case 0xc4:                              /* pinsrw mm, r32/m16, imm */
        d += 2 * (c & 3);
        if (!b->mem) { e(LI(0, d)); e(STHBRX(R(b->rm), RCPU, 0)); }
        else { a = ea_in(b); e(LHZ(3, 0, a)); e(STH(3, d, RCPU)); }
        return 1;
    case 0xc5:                              /* pextrw r32, mm, imm */
        e(LI(0, xmm_off(b->rm) + 2 * (c & 3))); e(LHBRX(R(b->reg), RCPU, 0));
        return 1;
    case 0x68: case 0x69: case 0x6a:        /* punpckh*: the low form on the high halves (bytes 4..7) */
        vx_load(0, b->reg);
        vsrc_load(1, b);
        e(VSLDOI(0, 0, 0, 4)); e(VSLDOI(1, 1, 1, 4));
        e(op == 0x68 ? VMRGHB(0, 0, 1) : op == 0x69 ? VMRGHH(0, 0, 1) : VMRGHW(0, 0, 1));
        vx_store(0, b->reg);
        return 1;
    case 0x63: case 0x67: case 0x6b:        /* packs: d's lanes, then s's, into 8 bytes */
        vx_load(0, b->reg);
        vsrc_load(1, b);
        for (i = 0; i < 16; i++) m[i] = (uint8_t)(i < 8 ? i : 8 + i);     /* d's 8 bytes, then s's */
        vconst(2, m); e(VPERM(0, 0, 1, 2));
        vconst(7, g_vrev); e(VPERM(0, 0, 0, 7));
        e(op == 0x63 ? VPKSHSS(0, 0, 0) : op == 0x67 ? VPKSHUS(0, 0, 0) : VPKSWSS(0, 0, 0));
        e(VPERM(0, 0, 0, 7));
        vx_store(0, b->reg);
        return 1;
    }
    return 0;
}

static void gen_sse_on(const Insn *b)
{
    int op = b->op & 0xff, d = xmm_off(b->reg), ra, rb, a, i, c = (int)(b->imm & 0xff);
    int gpr_rm = !b->mem && (op == 0x6e || op == 0x7e || op == 0xc4);   /* rm is an integer register */
    int digit = op == 0x71 || op == 0x72 || op == 0x73;                   /* reg is part of the opcode */
    uint8_t m[16];
    if (g_mmx) {
        if (gen_mmx_own(b, op, d, c)) return;
    } else {
        if (!digit) xf_forget(b->reg);   /* AltiVec works on Cpu.xmm */
        if (!b->mem && !gpr_rm) xf_forget(b->rm);
    }
    switch (op) {
    case 0x6f: case 0x10:                   /* xmm <- xmm/m128 */
        if (b->mem && (b->rep || op == 0x10)) {     /* movdqu, movups: raw bytes, without alignment */
            a = ea_in(b);
            for (i = 0; i < 4; i++) e(LWZ(3 + i, 4 * i, a));
            for (i = 0; i < 4; i++) e(STW(3 + i, d + 4 * i, RCPU));
            return;
        }
        vsrc_load(0, b);
        vx_store(0, b->reg);
        return;
    case 0x7f: case 0x11: case 0xe7:        /* xmm/m128 <- xmm */
        if (b->mem && (b->rep || op == 0x11)) {
            for (i = 0; i < 4; i++) e(LWZ(3 + i, d + 4 * i, RCPU));
            a = ea_in(b);
            for (i = 0; i < 4; i++) e(STW(3 + i, 4 * i, a));
            after_save(b, 16, 0, a, -1);
            return;
        }
        vx_load(0, b->reg);
        if (b->mem) { ea_pair(b, 1, &ra, &rb); e(STVX(0, ra, rb)); after_save(b, 16, ra, rb, -1); }
        else vx_store(0, b->rm);
        return;
    case 0xd7:                              /* pmovmskb r32, xmm: top bit of each byte */
        vx_load(0, b->rm);
        e(VSPLTISB(1, 0));
        e(VCMPGTSB(0, 1, 0));               /* 0xff where the byte is negative */
        load_imm(12, (uint32_t)(uintptr_t)g_weights); e(LVX(2, 0, 12));
        e(VAND(0, 0, 2));
        e(VSUM4UBS(0, 0, 1));               /* 4 bytes at a time into one word */
        e(LI(0, OFF(sse_tmp))); e(STVX(0, RCPU, 0));
        for (i = 0; i < 4; i++) e(LWZ(3 + i, OFF(sse_tmp) + 4 * i, RCPU));
        e(ADD(3, 3, 4)); e(ADD(5, 5, 6)); e(SLWI(5, 5, 8)); e(OR(R(b->reg), 3, 5));
        return;
    case 0x6e:                              /* movd xmm, r/m32: the rest becomes 0 */
        if (!b->mem) { e(LI(0, d)); e(STWBRX(R(b->rm), RCPU, 0)); }
        else if ((i = slot_reg(b, 4)) >= 0) { e(LI(0, d)); e(STWBRX(i, RCPU, 0)); }
        else { a = ea_in(b); e(LWZ(3, 0, a)); e(STW(3, d, RCPU)); }
        e(LI(3, 0));
        for (i = 4; i < 16; i += 4) e(STW(3, d + i, RCPU));
        return;
    case 0x7e:                              /* movd r/m32, xmm */
        if (!b->mem) { e(LI(0, d)); e(LWBRX(R(b->rm), RCPU, 0)); return; }
        e(LI(0, d)); e(LWBRX(3, RCPU, 0));
        ea_pair(b, 1, &ra, &rb);
        e(STWBRX(3, ra, rb));
        after_save(b, 4, ra, rb, 3);
        return;
    case 0xc4:                              /* pinsrw xmm, r32/m16, imm */
        d += 2 * (c & 7);
        if (!b->mem) { e(LI(0, d)); e(STHBRX(R(b->rm), RCPU, 0)); }
        else { a = ea_in(b); e(LHZ(3, 0, a)); e(STH(3, d, RCPU)); }
        return;
    case 0xc5:                              /* pextrw r32, xmm, imm */
        e(LI(0, xmm_off(b->rm) + 2 * (c & 7))); e(LHBRX(R(b->reg), RCPU, 0));
        return;
    case 0x70:                              /* pshufd, pshuflw (f2), pshufhw (f3): one vperm on the raw bytes */
        for (i = 0; i < 16; i++) m[i] = (uint8_t)i;
        if (b->p66) for (i = 0; i < 16; i++) m[i] = (uint8_t)(4 * ((c >> (i / 4 * 2)) & 3) + i % 4);
        else {
            int base = b->rep == 0xf3 ? 8 : 0;
            for (i = 0; i < 8; i++) m[base + i] = (uint8_t)(base + 2 * ((c >> (i / 2 * 2)) & 3) + i % 2);
        }
        vsrc_load(1, b);
        vconst(2, m);
        e(VPERM(0, 1, 1, 2));
        vx_store(0, b->reg);
        return;
    case 0xd1: case 0xd2: case 0xe1: case 0xe2: case 0xf1: case 0xf2: {
        /* shift 16/32-bit lanes by the low 64 bits of the source: psrlw/d, psraw/d, psllw/d. A count beyond the lane
           width: srl/sll give 0, sra the sign (count = width - 1) */
        int w = (op & 3) == 1, max = w ? 15 : 31, sra = (op & 0xf0) == 0xe0;
        if (b->mem) { a = ea_in(b); e(LWBRX(3, 0, a)); e(ADDI(4, a, 4)); e(LWBRX(4, 0, 4)); }
        else { e(LI(0, xmm_off(b->rm))); e(LWBRX(3, RCPU, 0)); e(LI(0, xmm_off(b->rm) + 4)); e(LWBRX(4, RCPU, 0)); }
        e(SUBFIC(5, 3, max)); e(SUBFE(5, 5, 5));        /* -1 if count > max (unsigned) */
        e(CNTLZW(6, 4)); e(SRWI(6, 6, 5)); e(ADDI(6, 6, -1));   /* -1 if the high word is not 0 */
        e(OR(5, 5, 6));
        if (sra) { e(ANDC(3, 3, 5)); e(LI(6, max)); e(AND(6, 6, 5)); e(OR(3, 3, 6)); }   /* (CR stays) */
        e(NOR(5, 5, 5));                    /* keep mask: 0 if the result is 0 */
        e(STW(3, OFF(sse_tmp), RCPU)); e(STW(5, OFF(sse_tmp) + 4, RCPU));
        e(LI(0, OFF(sse_tmp))); e(LVX(1, RCPU, 0));
        e(w ? VSPLTH(2, 1, 1) : VSPLTW(2, 1, 0));
        vx_load(0, b->reg);
        vconst(7, g_vrev); e(VPERM(0, 0, 0, 7));
        if (w) e((op & 0xf0) == 0xd0 ? VSRH(0, 0, 2) : sra ? VSRAH(0, 0, 2) : VSLH(0, 0, 2));
        else e((op & 0xf0) == 0xd0 ? VSRW(0, 0, 2) : sra ? VSRAW(0, 0, 2) : VSLW(0, 0, 2));
        if (!sra) { e(VSPLTW(3, 1, 1)); e(VAND(0, 0, 3)); }
        e(VPERM(0, 0, 0, 7));
        vx_store(0, b->reg);
        return;
    }
    case 0x71: case 0x72:                   /* shift 16/32-bit lanes by a constant: reg 2 srl, 4 sra, 6 sll */
        i = op == 0x71 ? 15 : 31;
        if (c > i && b->reg != 4) { e(VSPLTISB(0, 0)); vx_store(0, b->rm); return; }
        if (c > i) c = i;
        if (!c) return;
        vx_load(0, b->rm);
        vconst(7, g_vrev); e(VPERM(0, 0, 0, 7));
        if (op == 0x71) {
            e(VSPLTISH(1, c));
            e(b->reg == 2 ? VSRH(0, 0, 1) : b->reg == 4 ? VSRAH(0, 0, 1) : VSLH(0, 0, 1));
        } else {
            e(VSPLTISW(1, c < 16 ? c : c - 32));    /* the shift uses the low 5 bits */
            e(b->reg == 2 ? VSRW(0, 0, 1) : b->reg == 4 ? VSRAW(0, 0, 1) : VSLW(0, 0, 1));
        }
        e(VPERM(0, 0, 0, 7));
        vx_store(0, b->rm);
        return;
    case 0x73:                              /* 2 psrlq, 6 psllq (bits), 3 psrldq, 7 pslldq (bytes) */
        if (b->reg == 2 || b->reg == 6) {
            if (c > 63) { e(VSPLTISB(0, 0)); vx_store(0, b->rm); return; }
            if (c) gen_shift_q(b->rm, c, b->reg == 6);
            return;
        }
        if (c > 15) { e(VSPLTISB(0, 0)); vx_store(0, b->rm); return; }
        if (!c) return;
        vx_load(0, b->rm);
        e(VSPLTISB(1, 0));
        e(b->reg == 3 ? VSLDOI(0, 0, 1, c) : VSLDOI(0, 1, 0, 16 - c));     /* raw byte i <- i + c, i - c */
        vx_store(0, b->rm);
        return;
    case 0x5b:                              /* cvtdq2ps; cvtps2dq (66), cvttps2dq (f3): 0x80000000 if out of range */
        vsrc_load(1, b);
        vconst(7, g_vrev); e(VPERM(1, 1, 1, 7));
        if (!b->p66 && !b->rep) e(VCFSX(0, 1, 0));
        else {
            if (b->p66) e(VRFIN(1, 1));     /* rounding of MXCSR (nearest, the default) */
            e(VCTSXS(0, 1, 0));
            vconst(2, g_v2p31);
            e(VCMPGTFP(3, 2, 1));            /* in range: x < 2^31 (false for NaN; below -2^31 vctsxs saturates right) */
            e(VSPLTISW(4, -1)); e(VSLW(4, 4, 4));
            e(VSEL(0, 4, 0, 3));
        }
        e(VPERM(0, 0, 0, 7));
        vx_store(0, b->reg);
        return;
    }
    /* xmm op= xmm/m128 (memory must be aligned for SSE) */
    vx_load(0, b->reg);
    vsrc_load(1, b);
    switch (op) {                           /* raw form: lanes stay together, byte-wise */
    case 0x60: e(VMRGHB(0, 0, 1)); break;  /* punpckl*: lanes of the low halves alternately */
    case 0x61: e(VMRGHH(0, 0, 1)); break;
    case 0x62: case 0x14: e(VMRGHW(0, 0, 1)); break;
    case 0x68: e(VMRGLB(0, 0, 1)); break;  /* punpckh* */
    case 0x69: e(VMRGLH(0, 0, 1)); break;
    case 0x6a: case 0x15: e(VMRGLW(0, 0, 1)); break;
    case 0x6c: case 0x6d:                   /* punpcklqdq, punpckhqdq */
        for (i = 0; i < 16; i++) m[i] = (uint8_t)((i < 8 ? 0 : 16) + (op == 0x6d ? 8 : 0) + i % 8);
        vconst(2, m); e(VPERM(0, 0, 1, 2));
        break;
    case 0xc6:                              /* shufps: lanes 0, 1 from the destination, 2, 3 from the source */
        for (i = 0; i < 16; i++) m[i] = (uint8_t)((i < 8 ? 0 : 16) + 4 * ((c >> (i / 4 * 2)) & 3) + i % 4);
        vconst(2, m); e(VPERM(0, 0, 1, 2));
        break;
    case 0x64: e(VCMPGTSB(0, 0, 1)); break;
    case 0x74: e(VCMPEQUB(0, 0, 1)); break;
    case 0x75: e(VCMPEQUH(0, 0, 1)); break;
    case 0x76: e(VCMPEQUW(0, 0, 1)); break;
    case 0xd8: e(VSUBUBS(0, 0, 1)); break;
    case 0xda: e(VMINUB(0, 0, 1)); break;
    case 0xdb: e(VAND(0, 0, 1)); break;
    case 0xdc: e(VADDUBS(0, 0, 1)); break;
    case 0xde: e(VMAXUB(0, 0, 1)); break;
    case 0xdf: e(VANDC(0, 1, 0)); break;   /* ~d & s */
    case 0xe0: e(VAVGUB(0, 0, 1)); break;
    case 0xe8: e(VSUBSBS(0, 0, 1)); break;
    case 0xeb: e(VOR(0, 0, 1)); break;
    case 0xec: e(VADDSBS(0, 0, 1)); break;
    case 0xef: e(VXOR(0, 0, 1)); break;
    case 0xf8: e(VSUBUBM(0, 0, 1)); break;
    case 0xfc: e(VADDUBM(0, 0, 1)); break;
    case 0xf6:                              /* psadbw: per 64-bit lane the sum of |d - s| over its 8 bytes */
        e(VSUBUBS(2, 0, 1)); e(VSUBUBS(3, 1, 0)); e(VOR(0, 2, 3));
        e(VSPLTISB(1, 0));
        e(VSUM4UBS(0, 0, 1)); e(VSUM2SWS(0, 0, 1));    /* sum as 64-bit host value per half */
        vconst(2, g_vrev8); e(VPERM(0, 0, 0, 2));
        break;
    default:                                /* reversed form: lanes in host byte order */
        vconst(7, g_vrev);
        e(VPERM(0, 0, 0, 7)); e(VPERM(1, 1, 1, 7));
        switch (op) {
        case 0x63: e(VPKSHSS(0, 1, 0)); break;     /* packs: source first (it lands in the high half) */
        case 0x67: e(VPKSHUS(0, 1, 0)); break;
        case 0x6b: e(VPKSWSS(0, 1, 0)); break;
        case 0x65: e(VCMPGTSH(0, 0, 1)); break;
        case 0x66: e(VCMPGTSW(0, 0, 1)); break;
        case 0xd5: e(VSPLTISB(2, 0)); e(VMLADDUHM(0, 0, 1, 2)); break;   /* pmullw */
        case 0xe4: case 0xe5:               /* pmulhuw, pmulhw: high halves of the even and odd products */
            e(op == 0xe5 ? VMULESH(2, 0, 1) : VMULEUH(2, 0, 1));
            e(op == 0xe5 ? VMULOSH(3, 0, 1) : VMULOUH(3, 0, 1));
            vconst(4, g_vmulh); e(VPERM(0, 2, 3, 4));
            break;
        case 0xf5: e(VSPLTISB(2, 0)); e(VMSUMSHM(0, 0, 1, 2)); break;    /* pmaddwd */
        case 0xd9: e(VSUBUHS(0, 0, 1)); break;
        case 0xdd: e(VADDUHS(0, 0, 1)); break;
        case 0xe3: e(VAVGUH(0, 0, 1)); break;
        case 0xe9: e(VSUBSHS(0, 0, 1)); break;
        case 0xea: e(VMINSH(0, 0, 1)); break;
        case 0xed: e(VADDSHS(0, 0, 1)); break;
        case 0xee: e(VMAXSH(0, 0, 1)); break;
        case 0xf9: e(VSUBUHM(0, 0, 1)); break;
        case 0xfa: e(VSUBUWM(0, 0, 1)); break;
        case 0xfd: e(VADDUHM(0, 0, 1)); break;
        case 0xfe: e(VADDUWM(0, 0, 1)); break;
        case 0x58: e(VADDFP(0, 0, 1)); break;
        case 0x5c: e(VSUBFP(0, 0, 1)); break;
        case 0x59: e(VSPLTISW(2, -1)); e(VSLW(2, 2, 2)); e(VMADDFP(0, 0, 1, 2)); break;  /* d * s + (-0): one rounding */
        case 0x5d: e(VCMPGTFP(2, 1, 0)); e(VSEL(0, 1, 0, 2)); break;   /* minps: d < s ? d : s (NaN, +-0: s) */
        case 0x5f: e(VCMPGTFP(2, 0, 1)); e(VSEL(0, 1, 0, 2)); break;   /* maxps: d > s ? d : s */
        }
        e(VPERM(0, 0, 0, 7));
        break;
    }
    vx_store(0, b->reg);
}

/* ---------- Constants from read-only memory ----------
 * If a block reads floating-point values through a register it never changes (PIC base ebx, also through
 * lea eax,[ebx+d]) from a read-only region, the value is known at translation time. It goes into a pool in the
 * host and is loaded with one lfd/lfs, without the detour lwbrx/stw/lfd (lfd after two stw costs about 22 cycles on
 * the G4). A guard at the block start compares the register with its value at translation time; if it differs, the
 * block is retranslated without prediction. mprotect/munmap flush the whole cache anyway. */
#define CPOOL 16384
static uint64_t g_cpool[CPOOL];                 /* values in host byte order (float: in the upper 32 bits) */
static uint16_t g_cpool_h[2 * CPOOL];           /* hash table: index + 1 */
static int g_cpool_n;
static uint32_t g_no_spec[1024];              /* blocks whose guard failed once */
static unsigned long g_st_const, g_st_watchdog, g_st_watchdog_miss, g_st_masks;

static void cpool_flush(void)
{
    g_cpool_n = 0;
    memset(g_cpool_h, 0, sizeof g_cpool_h);
}

static int cpool(uint64_t v)
{
    uint32_t h = ((uint32_t)(v >> 32) * 2654435761u ^ (uint32_t)v * 40503u) & (2 * CPOOL - 1);
    while (g_cpool_h[h]) {
        if (g_cpool[g_cpool_h[h] - 1] == v) return g_cpool_h[h] - 1;
        h = (h + 1) & (2 * CPOOL - 1);
    }
    if (g_cpool_n >= CPOOL) return -1;
    g_cpool[g_cpool_n] = v;
    g_cpool_h[h] = (uint16_t)(g_cpool_n + 1);
    return g_cpool_n++;
}

/* constant of the current instruction (g_kaddr) as art into FPR f; 0: pool full (nothing generated) */
static int pool_load_at(int f, uint32_t ka, char kind);
static int pool_load(int f, char kind) { return pool_load_at(f, g_kaddr, kind); }
static int pool_load_at(int f, uint32_t ka, char kind)
{
    uint64_t v = kind == 'D' ? ld64(ka) : (uint64_t)ld32(ka) << 32;
    int k = cpool(v);
    uint32_t addr;
    if (k < 0) return 0;
    addr = (uint32_t)(uintptr_t)&g_cpool[k];
    e(LIS(12, (addr + 0x8000) >> 16));
    e(kind == 'D' ? LFD(f, addr & 0xffff, 12) : LFS(f, addr & 0xffff, 12));
    g_st_const++;
    return 1;
}

static int no_spec(uint32_t eip)
{
    uint32_t h = (eip * 2654435761u) >> 22, i;
    for (i = 0; i < 1024 && g_no_spec[h]; i++, h = (h + 1) & 1023) if (g_no_spec[h] == eip) return 1;
    return 0;
}

static void no_spec_note(uint32_t eip)
{
    uint32_t h = (eip * 2654435761u) >> 22, i;
    for (i = 0; i < 1024; i++, h = (h + 1) & 1023)
        if (!g_no_spec[h] || g_no_spec[h] == eip) { g_no_spec[h] = eip; return; }
}

/* blocks whose check at the loop entry failed once (then slots go to memory immediately again) */
static uint32_t g_no_ret_m[1024];

static int no_ret(uint32_t eip)
{
    uint32_t h = (eip * 2654435761u) >> 22, i;
    for (i = 0; i < 1024 && g_no_ret_m[h]; i++, h = (h + 1) & 1023) if (g_no_ret_m[h] == eip) return 1;
    return 0;
}

static void no_ret_note(uint32_t eip)
{
    uint32_t h = (eip * 2654435761u) >> 22, i;
    for (i = 0; i < 1024; i++, h = (h + 1) & 1023)
        if (!g_no_ret_m[h] || g_no_ret_m[h] == eip) { g_no_ret_m[h] = eip; return; }
}

/* size of the floating-point load from memory that gen_fp/gen_x87 do themselves (0: none) */
static int fp_load_size(const Insn *b)
{
    int op = b->op;
    if (!b->mem) return 0;
    if (x87_can(b)) {
        if (op == 0xd8 || (op == 0xd9 && b->reg == 0)) return 4;
        if (op == 0xdc || (op == 0xdd && b->reg == 0)) return 8;
        return 0;
    }
    if (pd_slot_op(b)) return 16;          /* packed: two doubles through one storage slot */
    if (op >= OP_0F && pd_can(b)) {
        int o = op & 0xff;
        if (o == 0x12 || o == 0x16 || o == 0x7e) return 8;
        if ((o == 0x5d || o == 0x5f || o == 0xc2) && b->rep == 0xf2) return 8;
        return 0;
    }
    if (op < OP_0F || !fp_can(b)) return 0;
    switch (op & 0xff) {
    case 0x10: case 0x58: case 0x59: case 0x5c: case 0x5e: case 0x51: case 0x5a: case 0x2c: case 0x2d:
        return b->rep == 0xf2 ? 8 : 4;
    case 0x2e: case 0x2f:
        return b->p66 ? 8 : 4;
    case 0x54: case 0x57:
        return 16;
    }
    return 0;
}

/* xorpd/xorps with sign mask or andpd/andps with abs mask in the low element (becomes fneg/fabs) */
static int mask_fits(uint32_t a, const Insn *b)
{
    int x = (b->op & 0xff) == 0x57;
    if (b->p66) { uint64_t m = ld64(a); return m == (x ? 0x8000000000000000ull : 0x7fffffffffffffffull); }
    return ld32(a) == (x ? 0x80000000u : 0x7fffffffu);
}

/* Memory operands that are known at translation time; returns the registers to guard (bit mask). At targets of
   jumps within the block (inner_target) several paths meet: there only what stays unchanged in the whole block
   still holds. */
static uint8_t const_predict(const Insn *b, const int *interp, const uint8_t *inner_target, int n, uint32_t *cval)
{
    uint8_t loop_w = 0, upto_w = 0, watchdog = 0, wm[MAX_INSNS];
    int symr[8], i, r, tail = 0;
    int32_t symo[8];
    memset(cval, 0, (size_t)n * sizeof *cval);
    for (i = 0; i < n && (interp[i] || !fp_load_size(&b[i])); i++) ;
    if (i == n) return 0;                       /* no floating-point load: nothing to do (startup stays fast) */
    for (i = 0; i < n; i++) {                   /* write_mask is conservative for unknown instructions (all) */
        const Insn *x = &b[i];
        wm[i] = write_mask(x);
        /* a call returns with eax, ecx, edx changed (the callee keeps ebx, esi, edi, ebp: i386 calling convention) */
        if (x->op == 0xe8 || x->op == 0x9a || (x->op == 0xff && (x->reg == 2 || x->reg == 3)))
            wm[i] |= (uint8_t)(1u << EAX | 1u << ECX | 1u << EDX);
        /* jump backwards (into a loop of the block, or of an inlined function): what is written up to here can flow
           back */
        if ((x->op == 0xeb || x->op == 0xe9 || (x->op >= 0x70 && x->op <= 0x7f) || (x->op >= 0xe0 && x->op <= 0xe3) ||
             (x->op >= (OP_0F | 0x80) && x->op <= (OP_0F | 0x8f))) && x->eip + x->len + x->imm <= x->eip)
            tail = i + 1;
        if (x->op == 0xff && (x->reg == 4 || x->reg == 5)) tail = i + 1;     /* switch table: targets anywhere */
    }
    for (i = 0; i < tail; i++) loop_w |= wm[i];
    /* register = value at entry + offset (symr < 0: unknown). Until an instruction writes it, a register still has
       its entry value; at a jump target inside the block the paths meet: unknown is what is written before it or in
       a loop (up to the last backward jump; the code after it never flows back, an epilogue's pop does not count) */
    for (r = 0; r < 8; r++) { symr[r] = (loop_w >> r & 1) ? -1 : r; symo[r] = 0; }
    for (i = 0; i < n; i++) {
        const Insn *x = &b[i];
        int sz = interp[i] ? 0 : fp_load_size(x);
        if (inner_target[i]) for (r = 0; r < 8; r++) if ((loop_w | upto_w) >> r & 1) symr[r] = -1;
        upto_w |= wm[i];
        if (sz && !x->seg && x->base >= 0 && symr[x->base] >= 0 && (x->index < 0 || symr[x->index] >= 0)) {
            uint32_t a = g_cpu->r[symr[x->base]] + (uint32_t)symo[x->base] + x->disp;
            if (x->index >= 0) a += (g_cpu->r[symr[x->index]] + (uint32_t)symo[x->index]) << x->scale;
            if (a - COMMPAGE_BASE >= COMMPAGE_SIZE && a + (uint32_t)sz - COMMPAGE_BASE >= COMMPAGE_SIZE &&
                page_readonly(a) && page_readonly(a + (uint32_t)sz - 1) && (sz != 16 || pd_slot_op(x) || mask_fits(a, x))) {
                cval[i] = a;
                watchdog |= (uint8_t)(1 << symr[x->base]);
                if (x->index >= 0) watchdog |= (uint8_t)(1 << symr[x->index]);
            }
        }
        if (interp[i]) { for (r = 0; r < 8; r++) if (wm[i] >> r & 1) symr[r] = -1; continue; }
        if (x->op == 0x8d && x->opsz == 4 && !x->seg && x->index < 0 && x->base >= 0 && symr[x->base] >= 0) {
            int s = x->base;                   /* lea r, [s + d] */
            symo[x->reg] = symo[s] + (int32_t)x->disp;
            symr[x->reg] = symr[s];
            continue;
        }
        if ((x->op == 0x89 || x->op == 0x8b) && !x->mem && x->opsz == 4) {
            int z = x->op == 0x89 ? x->rm : x->reg, q = x->op == 0x89 ? x->reg : x->rm;
            symr[z] = symr[q]; symo[z] = symo[q];
            continue;
        }
        for (r = 0; r < 8; r++) if (wm[i] >> r & 1) symr[r] = -1;
    }
    return watchdog;
}

/* ---------- Hoisting array accesses ----------
 * doubles from arrays reach the FPU through lwbrx/stw/lfd; the lfd right after the stw costs ~22 cycles on the G4,
 * only a distance of ~40 cycles removes that. In loop blocks (exactly one backward jump, address registers only
 * with a fixed step, no memory writes in the body) each iteration puts the values for VL_DEPTH iterations later
 * into a ring buffer; loads use lfd from the ring. Ring pointer in a free r28..r31; before the head the first
 * VL_DEPTH iterations are loaded. Prefetched addresses can lie beyond the array: if such an lwbrx hits invalid
 * memory, the fault handler skips it (jit_prefetch_error); the value is then never needed. */
#define VL_DEPTH 4
#define VL_STREAMS 4
#define VL_RINGS 2048
#define VL_PCS 32768
static double g_vl_ring[VL_RINGS][VL_DEPTH * VL_STREAMS] __attribute__((aligned(128)));  /* 128 bytes per block */
static int g_vl_nring;
static uint32_t g_vl_pc[VL_PCS];                /* sites of the prefetching lwbrx (hash table) */
static int g_vl_npc;
static uint32_t g_vl_new[256];                  /* such sites in the block being translated (entered only at the end) */
static int g_vl_nnew, g_vl_overflow;
static int g_vl_reg;                            /* ring pointer (0: no hoisting in this block) */
static int32_t g_vl_step[VL_STREAMS];
static char g_vl_kind[VL_STREAMS];               /* 'm' memory (double or float), 'u' conversion integer -> double */
static int8_t g_vl_size[VL_STREAMS];             /* 'm': 8 double, 4 float */
static int8_t g_vl_src[VL_STREAMS];          /* 'u': counter register */
static uint32_t g_vl_mask[VL_STREAMS];         /* 'u': 0 whole, 0xff/0xffff after movzbl/movzwl */
static int g_vl_nloaded;
static uint32_t g_vl_forbidden = 1;              /* eip that is retranslated without hoisting */
static unsigned long g_st_prefetch, g_st_vl_streams, g_st_vl_error;

static void vl_flush(void)
{
    g_vl_nring = 0;
    g_vl_npc = 0;
    memset(g_vl_pc, 0, sizeof g_vl_pc);
}

static uint32_t vl_scatter(uint32_t pc) { return ((pc >> 2) * 2654435761u) >> 17; }

static void vl_pc_add(uint32_t pc)
{
    uint32_t h = vl_scatter(pc);
    while (g_vl_pc[h]) h = (h + 1) & (VL_PCS - 1);
    g_vl_pc[h] = pc;
    g_vl_npc++;
}

/* memory fault: if it is at a prefetching lwbrx, skip the instruction (1). */
int jit_prefetch_error(void *ctx)
{
    ucontext_t *u = (ucontext_t *)ctx;
    uint32_t pc, h;
    if (!g_vl_npc || !u) return 0;
#if __DARWIN_UNIX03
    pc = u->uc_mcontext->__ss.__srr0;
#else
    pc = u->uc_mcontext->ss.srr0;
#endif
    for (h = vl_scatter(pc); g_vl_pc[h]; h = (h + 1) & (VL_PCS - 1))
        if (g_vl_pc[h] == pc) {
#if __DARWIN_UNIX03
            u->uc_mcontext->__ss.__srr0 = pc + 4;
#else
            u->uc_mcontext->ss.srr0 = pc + 4;
#endif
            g_st_vl_error++;
            return 1;
        }
    return 0;
}

/* fault dump for the signal handler: cache, registers, return ring, host instructions around srr0 */
void jit_crashdump(void *ctx)
{
    ucontext_t *u = (ucontext_t *)ctx;
    uint32_t r[32], pc, lr, ctr, *w;
    int i;
    if (!u || !g_cache) return;
    for (i = 0; i < 32; i++)
#if __DARWIN_UNIX03
        r[i] = (&u->uc_mcontext->__ss.__r0)[i];
    pc = u->uc_mcontext->__ss.__srr0; lr = u->uc_mcontext->__ss.__lr; ctr = u->uc_mcontext->__ss.__ctr;
#else
        r[i] = (&u->uc_mcontext->ss.r0)[i];
    pc = u->uc_mcontext->ss.srr0; lr = u->uc_mcontext->ss.lr; ctr = u->uc_mcontext->ss.ctr;
#endif
    fprintf(stderr, "rosetta: cache %08x..%08x (free from %08x), srr0 %s, lr %s, ctr %s\n", (unsigned)(uintptr_t)g_cache,
            (unsigned)(uintptr_t)g_cache_end, (unsigned)(uintptr_t)g_free,
            pc >= (uint32_t)(uintptr_t)g_cache && pc < (uint32_t)(uintptr_t)g_free ? "in code" : "outside",
            lr >= (uint32_t)(uintptr_t)g_cache && lr < (uint32_t)(uintptr_t)g_free ? "in code" : "outside",
            ctr >= (uint32_t)(uintptr_t)g_cache && ctr < (uint32_t)(uintptr_t)g_free ? "in code" : "outside");
    fprintf(stderr, "rosetta: r0 %08x r1 %08x r2 %08x r3 %08x r4 %08x r12 %08x r13 %08x r22 %08x r23 %08x\n",
            r[0], r[1], r[2], r[3], r[4], r[12], r[13], r[22], r[23]);
    if (r[13] >= g_shadow_base && r[13] < g_shadow_base + 64u * 1024 * 8) {
        fprintf(stderr, "rosetta: ring (r13 = base + %u):", r[13] - g_shadow_base);
        for (w = (uint32_t *)(uintptr_t)(r[13] - 16), i = 0; i < 12; i++) fprintf(stderr, " %08x", w[i]);
        fprintf(stderr, "\n");
    }
    for (i = 0; i < 2; i++) {
        uint32_t a = i ? ctr : lr;
        if (a >= (uint32_t)(uintptr_t)g_cache + 32 && a < (uint32_t)(uintptr_t)g_free) {
            int k;
            fprintf(stderr, "rosetta: code before %s %08x:", i ? "ctr" : "lr", a);
            for (k = -8; k < 4; k++) fprintf(stderr, " %08x", ((uint32_t *)(uintptr_t)a)[k]);
            fprintf(stderr, "\n");
        }
    }
}
static void vl_lwbrx(int d, int a, int b)
{
    if (g_vl_nnew < 256) g_vl_new[g_vl_nnew++] = (uint32_t)(uintptr_t)p; else g_vl_overflow = 1;
    e(LWBRX(d, a, b));
}

/* double (size 8) or float (4) at memory operand t (offset already adjusted) byte-swapped to ring slot o (relative
   to the ring pointer) */
static void vl_put(const Insn *t, int o, int size)
{
    int a = ea_in(t);
    if (size == 4) {
        vl_lwbrx(3, 0, a);
        e(STW(3, o, g_vl_reg));
        return;
    }
    e(LI(0, 4));
    vl_lwbrx(3, a, 0);
    vl_lwbrx(4, 0, a);
    e(STW(3, o, g_vl_reg));
    e(STW(4, o + 4, g_vl_reg));
}

/* value of this iteration from the ring into FPR f, then fill the same slot for VL_DEPTH iterations later */
static void vl_load(int f, const Insn *b)
{
    int s = g_vl_cur - 1;
    Insn t = *b;
    e(g_vl_size[s] == 4 ? LFS(f, 8 * s, g_vl_reg) : LFD(f, 8 * s, g_vl_reg));
    t.disp += (uint32_t)(VL_DEPTH * g_vl_step[s]);
    vl_put(&t, 8 * s, g_vl_size[s]);
    g_vl_nloaded++;
}

/* conversion stream s: double bits (2^52 + 2^31 + value) for the counter value + extra to ring slot o */
static void vl_conv_put(int s, int32_t extra, int o)
{
    e(ADDI(3, R(g_vl_src[s]), extra));
    if (g_vl_mask[s] == 0xff) e(RLWINM(3, 3, 0, 24, 31));
    else if (g_vl_mask[s] == 0xffff) e(RLWINM(3, 3, 0, 16, 31));
    e(XORIS(3, 3, 0x8000));
    e(LIS(4, 0x4330));
    e(STW(4, o, g_vl_reg));
    e(STW(3, o + 4, g_vl_reg));
}

/* cvtsi2sd from the ring: value of this iteration into FPR f (exact), fill the slot for VL_DEPTH iterations later */
static void vl_convert(int f)
{
    int s = g_vl_cur - 1;
    e(LFD(0, 8 * s, g_vl_reg));
    e(FSUB(f, 0, F_MAGIC));
    vl_conv_put(s, VL_DEPTH * g_vl_step[s], 8 * s);
    g_vl_nloaded++;
}

/* ring pointer to the next iteration (ring 128 bytes, aligned to its size) */
static void vl_cont(void)
{
    e(ADDI(0, g_vl_reg, 8 * VL_STREAMS));
    e(RLWIMI(g_vl_reg, 0, 0, 25, 31));
}

/* r changes through b only by a fixed amount (add/sub imm, inc/dec, lea r,[r+d]) */
static int simple_change(const Insn *b, int r, int32_t *d)
{
    int op = b->op;
    if (b->opsz != 4) return 0;
    if (op == 0x40 + r) { *d = 1; return 1; }
    if (op == 0x48 + r) { *d = -1; return 1; }
    if ((op == 0x81 || op == 0x83) && !b->mem && b->rm == r && (b->reg == 0 || b->reg == 5)) {
        *d = b->reg == 0 ? (int32_t)b->imm : -(int32_t)b->imm;
        return 1;
    }
    if ((op == 0x05 || op == 0x2d) && r == EAX) { *d = op == 0x05 ? (int32_t)b->imm : -(int32_t)b->imm; return 1; }
    if (op == 0x8d && !b->seg && b->reg == r && b->base == r && b->index < 0) { *d = (int32_t)b->disp; return 1; }
    return 0;
}

/* instruction certainly does not write memory (0 when in doubt) */
static int no_memwrite(const Insn *b)
{
    int op = b->op, o = op & 0xff;
    if (op >= 0x50 && op <= 0x57) return 0;
    switch (op) {
    case 0x68: case 0x6a: case 0x9a: case 0x9c: case 0xa2: case 0xa3: case 0xa4: case 0xa5: case 0xaa: case 0xab:
    case 0xc8: case 0xe8:
        return 0;
    }
    if (op == 0xff && (b->reg == 2 || b->reg == 3 || b->reg == 6)) return 0;
    if (!b->mem) return op != 0xcd && op != (OP_0F | 0x34) && op != (OP_0F | 0x05);
    if (x87_can(b))
        return op == 0xd8 || op == 0xdc || ((op == 0xd9 || op == 0xdd || op == 0xdb || op == 0xdf) && b->reg == 0);
    if (op >= OP_0F && pd_can(b)) return o != 0x11 && o != 0x29 && o != 0x13 && o != 0x17 && o != 0xd6;
    if (op >= OP_0F && sse_can(b)) return o != 0x7f && o != 0x11 && o != 0xe7 && o != 0x7e;
    if (op >= OP_0F && fp_can(b)) return o != 0x11 && o != 0x29;
    switch (op) {
    case 0x8a: case 0x8b: case 0x8d: case 0x84: case 0x85: case 0x69: case 0x6b: case OP_0F | 0xaf:
    case OP_0F | 0xb6: case OP_0F | 0xb7: case OP_0F | 0xbe: case OP_0F | 0xbf:
        return 1;
    case 0x80: case 0x81: case 0x83:
        return b->reg == 7;
    case 0xf6: case 0xf7:
        return b->reg == 0 || b->reg >= 4;
    }
    if (op < 0x40 && (op & 7) < 6) return (op & 7) >= 2 || (op >> 3) == 7;
    return 0;
}

/* double or float load that goes through m_load or the load paths of gen_fp/gen_x87 (vl_load is there): its size
   (8 or 4), 0 if not */
static int vl_can(const Insn *b)
{
    int op = b->op, o = op & 0xff;
    if (!b->mem || b->seg) return 0;
    if (x87_can(b)) {
        if (op == 0xdc || (op == 0xdd && b->reg == 0)) return 8;
        if (op == 0xd8 || (op == 0xd9 && b->reg == 0)) return 4;
        return 0;
    }
    if (op < OP_0F || sse_can(b) || !fp_can(b) || (b->rep != 0xf2 && b->rep != 0xf3) || b->p66) return 0;
    if (o == 0x10 || o == 0x58 || o == 0x59 || o == 0x5c || o == 0x5e || o == 0x51 || o == 0x5a || o == 0x2c || o == 0x2d)
        return b->rep == 0xf2 ? 8 : 4;
    return 0;
}

/* the instruction's only memory write is a plain store of *size bytes to its memory operand (0: some other or an
   unknown write) */
static int store_size(const Insn *b, int *size)
{
    int op = b->op, o = op & 0xff;
    if (!b->mem || b->seg || b->lock) return 0;
    switch (op) {
    case 0x88: case 0xc6: *size = 1; return 1;
    case 0x89: case 0xc7: *size = b->opsz; return 1;
    case 0xd9: if (b->reg == 2 || b->reg == 3) { *size = 4; return 1; } return 0;   /* fst/fstp m32 */
    case 0xdd: if (b->reg == 2 || b->reg == 3) { *size = 8; return 1; } return 0;   /* fst/fstp m64 */
    }
    if (op >= OP_0F && o == 0x11 && !b->p66 && (b->rep == 0xf2 || b->rep == 0xf3)) {  /* movsd/movss m, xmm */
        *size = b->rep == 0xf2 ? 8 : 4;
        return 1;
    }
    return 0;
}

/* Choose streams: stream[i] = stream + 1, stream_i/pre_d per stream (instruction, address change from the head up to
   there); returns the number of streams and sets g_vl_reg */
static int prefetch_choose(const Insn *b, const int *kinds, const int *traced, const int *interp, const uint32_t *cval,
                           int n, uint32_t eip, int *stream, int *stream_i, int32_t *pre_d)
{
    int j = -1, i, r, k, ns = 0, ok[8], mem = 0, nst, st_i[8], st_sz[8];
    int32_t step[8], pre[8], d, st_pre[8];
    uint8_t wm[MAX_INSNS];
    g_vl_reg = 0;
    memset(stream, 0, (size_t)n * sizeof *stream);
    if (g_no_prefetch || eip == g_vl_forbidden || 28 + g_sl_n + g_k_n > 31) return 0;
    if (g_vl_nring >= VL_RINGS || g_vl_npc > VL_PCS / 2) return 0;
    for (i = 0; i < n; i++)
        if ((kinds[i] == JUMP || kinds[i] == JUMP_COND) && !traced[i] && b[i].eip + b[i].len + b[i].imm == eip) {
            if (j >= 0) return 0;               /* more than one backward jump: step per iteration not fixed */
            j = i;
        }
    if (j < 0) return 0;
    for (r = 0; r < 8; r++) { ok[r] = 1; step[r] = 0; pre[r] = 0; }
    nst = 0;
    for (i = 0; i <= j; i++) {
        int sz;
        if (interp[i]) return 0;
        if (!no_memwrite(&b[i])) {
            /* plain stores are allowed if no stream can overlap them (checked below); any other write: no streams */
            if (nst < 8 && store_size(&b[i], &sz)) { st_i[nst] = i; st_sz[nst] = sz; nst++; }
            else mem = 1;
        }
        wm[i] = write_mask(&b[i]);
        for (r = 0; r < 8; r++)
            if (wm[i] >> r & 1) { if (simple_change(&b[i], r, &d)) step[r] += d; else ok[r] = 0; }
    }
    {                                       /* offset of each store from the head (address registers advanced up to it) */
        int32_t pr[8];
        for (r = 0; r < 8; r++) pr[r] = 0;
        for (i = 0; i <= j; i++) {
            for (k = 0; k < nst; k++)
                if (st_i[k] == i) {
                    const Insn *x = &b[i];
                    st_pre[k] = (x->base >= 0 ? pr[x->base] : 0) + (x->index >= 0 ? pr[x->index] << x->scale : 0);
                }
            for (r = 0; r < 8; r++) if ((wm[i] >> r & 1) && simple_change(&b[i], r, &d)) pr[r] += d;
        }
    }
    for (i = 0; i <= j; i++) {
        const Insn *x = &b[i];
        int size = vl_can(x);
        if (ns < VL_STREAMS && !mem && !cval[i] && size && (x->base < 0 || ok[x->base]) &&
            (x->index < 0 || ok[x->index])) {
            int32_t st = (x->base >= 0 ? step[x->base] : 0) + (x->index >= 0 ? step[x->index] << x->scale : 0);
            int32_t vd = (x->base >= 0 ? pre[x->base] : 0) + (x->index >= 0 ? pre[x->index] << x->scale : 0);
            int clash = 0;
            /* A store hits a value of this stream between its prefetch (VL_DEPTH iterations earlier, at the stream's
               instruction) and its use: only stores with the same address registers can be ruled out. Relative to
               the stream's address in the same iteration a store lies at dd; k iterations before the use it
               covers [dd - k*st, dd - k*st + size of the store) */
            for (k = 0; k < nst && !clash; k++) {
                const Insn *w = &b[st_i[k]];
                int32_t dd = (int32_t)w->disp + st_pre[k] - ((int32_t)x->disp + vd), kk;
                int kmin = st_i[k] < i ? 0 : 1, kmax = st_i[k] > i ? VL_DEPTH : VL_DEPTH - 1;
                if (w->base != x->base || w->index != x->index || (w->index >= 0 && w->scale != x->scale)) { clash = 1; break; }
                for (kk = kmin; kk <= kmax; kk++) {
                    int32_t lo = dd - kk * st;
                    if (lo < size && lo + st_sz[k] > 0) { clash = 1; break; }
                }
            }
            if (!clash && st >= -(1 << 20) && st <= (1 << 20)) {
                g_vl_kind[ns] = 'm';
                g_vl_size[ns] = (int8_t)size;
                g_vl_step[ns] = st;
                stream_i[ns] = i;
                pre_d[ns] = vd;
                stream[i] = ++ns;
            }
        } else if (ns < VL_STREAMS && x->op == (OP_0F | 0x2a) && !x->mem && !sse_can(x) && fp_can(x)) {
            /* cvtsi2sd/ss from a counter (or movzbl/movzwl of a counter before it): bits into the ring in advance */
            int q = x->rm, k;
            uint32_t msk = 0;
            if (!ok[q]) {
                for (k = i - 1; k >= 0 && !(wm[k] >> x->rm & 1); k--) ;
                q = -1;
                if (k >= 0 && (b[k].op == (OP_0F | 0xb6) || b[k].op == (OP_0F | 0xb7)) && !b[k].mem && b[k].reg == x->rm &&
                    b[k].opsz == 4) {               /* (movzbw writes only the low half) */
                    int s = b[k].rm, m;
                    if (b[k].op == (OP_0F | 0xb6) && s >= 4) s = -1;     /* ah..bh: not */
                    if (s >= 0 && ok[s]) {
                        for (m = k + 1; m < i && !(wm[m] >> s & 1); m++) ;
                        if (m == i) { q = s; msk = b[k].op == (OP_0F | 0xb6) ? 0xff : 0xffff; }
                    }
                }
            }
            if (q >= 0 && step[q] >= -4096 && step[q] <= 4096) {
                g_vl_kind[ns] = 'u';
                g_vl_src[ns] = (int8_t)q;
                g_vl_mask[ns] = msk;
                g_vl_step[ns] = step[q];
                stream_i[ns] = i;
                pre_d[ns] = pre[q];
                stream[i] = ++ns;
            }
        }
        for (r = 0; r < 8; r++) if ((wm[i] >> r & 1) && simple_change(x, r, &d)) pre[r] += d;
    }
    if (ns) g_vl_reg = 28 + g_sl_n + g_k_n;
    return ns;
}

/* before the head: set the ring pointer and store the first VL_DEPTH iterations */
static void vl_prefetch_code(const Insn *b, const int *stream_i, const int32_t *pre_d, int ns)
{
    int s, k;
    load_imm(g_vl_reg, (uint32_t)(uintptr_t)g_vl_ring[g_vl_nring]);
    for (s = 0; s < ns; s++)
        for (k = 0; k < VL_DEPTH; k++) {
            if (g_vl_kind[s] == 'u') { vl_conv_put(s, pre_d[s] + k * g_vl_step[s], k * 8 * VL_STREAMS + 8 * s); continue; }
            {
                Insn t = b[stream_i[s]];
                t.disp += (uint32_t)(pre_d[s] + k * g_vl_step[s]);
                vl_put(&t, k * 8 * VL_STREAMS + 8 * s, g_vl_size[s]);
            }
        }
}

/* ---------- Floating-point store forwarding within the block ----------
 * If the block stores a double/float from an FPR, the translator remembers the value in a free FPR
 * (f2..f13) together with the address form (base, index, scale, offset, size). If the block reads the same form
 * again, the value comes from the FPR instead of through memory (~30 cycles on the G4). An entry becomes invalid
 * when an involved register changes, a memory write could hit it or C code uses the volatile FPRs. */
typedef struct { int8_t base, index; uint8_t scale, sz; int32_t disp; } FwForm;
#define FW_N 12
/* Entry k: value in f(2 + k) (fpr_ok) and/or as a host copy at shadow (offset to RCPU: Cpu.fw_shadow[k] or
   a slot of the floating-point store); the copy survives C calls that do not touch guest memory */
static struct { FwForm f; int valid, fpr_ok; int16_t shadow; } g_fw[FW_N];
static int g_fw_next, g_c_pure;
static unsigned long g_st_fw;

static void fw_flush(void) { int k; for (k = 0; k < FW_N; k++) g_fw[k].valid = 0; }

/* C call: FPRs f2..f13 are lost afterwards; without guest memory access (g_c_pure) the host copies stay */
static void fw_c_call(void)
{
    int k;
    if (!g_c_pure) { fw_flush(); return; }
    for (k = 0; k < FW_N; k++) {
        if (!g_fw[k].valid) continue;
        if (g_fw[k].fpr_ok && g_fw[k].shadow == (int16_t)(OFF(fw_shadow) + 8 * k))   /* write the copy only now */
            e(g_fw[k].f.sz == 8 ? STFD(2 + k, g_fw[k].shadow, RCPU) : STFS(2 + k, g_fw[k].shadow, RCPU));
        g_fw[k].fpr_ok = 0;
        if (!g_fw[k].shadow) g_fw[k].valid = 0;
    }
}

static FwForm fw_form(const Insn *b, int sz)
{
    FwForm f;
    f.base = b->base; f.index = b->index; f.scale = b->index >= 0 ? b->scale : 0; f.sz = (uint8_t)sz;
    f.disp = (int32_t)b->disp;
    return f;
}

/* FPR with the value at the address of b (size sz), otherwise -1 */
static int fw_find(const Insn *b, int sz)
{
    FwForm f;
    int k;
    if (!b->mem || b->seg || g_no_fw) return -1;
    f = fw_form(b, sz);
    for (k = 0; k < FW_N; k++) {
        FwForm *q = &g_fw[k].f;
        if (g_fw[k].valid && q->base == f.base && q->index == f.index && q->scale == f.scale && q->disp == f.disp &&
            q->sz == f.sz) {
            g_st_fw++;
            if (!g_fw[k].fpr_ok) {          /* after a C call: from the host copy (written long before, no stall) */
                e(sz == 8 ? LFD(2 + k, g_fw[k].shadow, RCPU) : LFS(2 + k, g_fw[k].shadow, RCPU));
                g_fw[k].fpr_ok = 1;
            }
            return 2 + k;
        }
    }
    return -1;
}

static int fw_stack(int base, int32_t d, int32_t *off);
static void fw_write_stack(int32_t lo, int32_t hi);

/* write to the address of b (at most sz bytes): whatever could overlap is invalid */
static void fw_write(const Insn *b, int sz)
{
    int k;
    int32_t o;
    if (!b->mem || b->seg) { fw_flush(); return; }
    if (!g_no_fw_stack && b->index < 0 && fw_stack(b->base, (int32_t)b->disp, &o)) {   /* stack: symbolic */
        fw_write_stack(o, o + sz);
        return;
    }
    for (k = 0; k < FW_N; k++) {
        FwForm *q = &g_fw[k].f;
        if (!g_fw[k].valid) continue;
        if (q->base == b->base && q->index == b->index && q->scale == (b->index >= 0 ? b->scale : 0) &&
            ((int32_t)b->disp + sz <= q->disp || q->disp + q->sz <= (int32_t)b->disp)) continue;   /* certainly separate */
        g_fw[k].valid = 0;
    }
}

/* allocate the next entry for the address of b (sz); returns its FPR (the caller fills it) or -1 */
static int fw_new(const Insn *b, int sz)
{
    int k;
    if (!b->mem || b->seg || g_no_fw) return -1;
    k = g_fw_next;
    g_fw_next = (g_fw_next + 1) % FW_N;
    g_fw[k].f = fw_form(b, sz);
    g_fw[k].valid = 1;
    g_fw[k].fpr_ok = 1;
    g_fw[k].shadow = (int16_t)(OFF(fw_shadow) + 8 * k);
    return 2 + k;
}

/* FPR q (from fw_new, already filled) also into its host copy */
static void fw_shadow_write(int q, int sz)
{
    (void)q; (void)sz;                      /* the copy is written only by fw_c_call, before a pure C call */
}

/* pure copy (movsd m,xmm with a pending value): the host copy is storage slot z (no FPR) */
static void fw_note_slot(const Insn *b, int sz, int z)
{
    int k;
    if (!b->mem || b->seg || g_no_fw) return;
    k = g_fw_next;
    g_fw_next = (g_fw_next + 1) % FW_N;
    g_fw[k].f = fw_form(b, sz);
    g_fw[k].valid = 1;
    g_fw[k].fpr_ok = 0;
    g_fw[k].shadow = (int16_t)z;
}

/* storage slot z is reused: entries whose copy was there lose it */
static void fw_slot_drop(int z)
{
    int k;
    for (k = 0; k < FW_N; k++)
        if (g_fw[k].valid && g_fw[k].shadow == z) { g_fw[k].shadow = 0; if (!g_fw[k].fpr_ok) g_fw[k].valid = 0; }
}

/* register r changes: entries with r in the address are invalid */
static void fw_register(int r)
{
    int k;
    for (k = 0; k < FW_N; k++) if (g_fw[k].valid && (g_fw[k].f.base == r || g_fw[k].f.index == r)) g_fw[k].valid = 0;
}

/* floating-point store that forwards by itself (movsd/movss m,xmm and fst/fstp m32/m64) */
static int fw_self(const Insn *b)
{
    int op = b->op;
    if (!b->mem) return 0;
    if (x87_can(b)) return (op == 0xd9 || op == 0xdd) && (b->reg == 2 || b->reg == 3);
    return op == (OP_0F | 0x11) && !sse_can(b) && fp_can(b);
}

/* bytes an instruction can write at its memory operand (x87 and unknown: 16 to be safe) */
static int write_size(const Insn *b)
{
    int sz, l, s;
    if (g_no_exact_sz || x87_can(b) || !mem_access(b, &sz, &l, &s) || !s || sz > 16) return 16;
    return sz;
}

/* Stack offset (relative to esp at the block head) of [basis + d], if stack forwarding knows esp/ebp (state before
   the current instruction) */
static int fw_stack(int base, int32_t d, int32_t *off)
{
    if (base == ESP && g_sw_esp_ok) { *off = g_sw_esp + d; return 1; }
    if (base == EBP && g_sw_ebp_ok) { *off = g_sw_ebp + d; return 1; }
    return 0;
}

/* write to the stack [lo, hi) (symbolic): entries on provably different stack slots stay */
static void fw_write_stack(int32_t lo, int32_t hi)
{
    int k;
    for (k = 0; k < FW_N; k++) {
        FwForm *q = &g_fw[k].f;
        int32_t o;
        if (!g_fw[k].valid) continue;
        if (q->index < 0 && fw_stack(q->base, q->disp, &o) && (o + q->sz <= lo || hi <= o)) continue;
        g_fw[k].valid = 0;
    }
}

/* before every instruction: what it writes (memory, registers) invalidates entries */
static void fw_pre_insn(const Insn *b, int interp, int verf)
{
    int r, k, op = b->op;
    int32_t o;
    for (k = 0; k < FW_N && !g_fw[k].valid; k++) ;
    if (k == FW_N) return;                  /* nothing remembered (the normal case): translation stays fast */
    if (verf == 5) return;                  /* direct math: writes no memory (c_call flushes afterwards) */
    if (interp) fw_flush();
    else if (!g_no_fw_stack && !b->mem && is_push(b) && op != 0x60 && g_sw_esp_ok)
        fw_write_stack(g_sw_esp - 4, g_sw_esp);            /* push, call: slot below esp */
    else if (!g_no_fw_stack && b->mem && !b->seg && b->index < 0 && !fw_self(b) && !no_memwrite(b) &&
             fw_stack(b->base, (int32_t)b->disp, &o))
        fw_write_stack(o, o + write_size(b));
    else if (!fw_self(b) && !no_memwrite(b)) fw_write(b, write_size(b));
    {
        uint8_t wm = interp ? 0xff : write_mask(b);
        for (r = 0; r < 8; r++) if (wm >> r & 1) fw_register(r);
    }
}

/* ---------- Forwarding through the Intel stack within the block ----------
 * esp/ebp are tracked symbolically (offset to esp at the block head). What the block writes to a stack slot
 * (push, mov [esp/ebp+d], return address of an inlined call), the translator remembers as a constant,
 * as a guest register (as long as it does not change) or as a copy in a free r28..r31. pop, leave and
 * mov r,[esp/ebp+d] then take the value without a load (load after store stalls the G4 ~8 cycles); the ret of an
 * inlined function is dropped entirely if its return slot provably holds the expected address. */
#define SW_N 8
/* kind 'k' constant, 'g' guest register, 'w' copy in host register reg; store: the slot got the value from a store
   in the block (reading it from memory would stall the G4: a 'g' entry then moves to a copy when its register
   changes) */
typedef struct { int32_t off; char valid, kind; int8_t reg; char store; uint32_t val, nr; char dirty; int16_t kill;
                 int32_t edisp; } SwEntry;
static SwEntry g_sw[SW_N];
/* Dead stores: a 32-bit store to [ebp+d] whose slot a later instruction of the block overwrites entirely (kill) before
   anything else can see it is left out; the entry is dirty (value only in its register) and is written to memory after
   all (sw_flush1: li r0,d; stwbrx) if it has to leave the table before kill. sw_killer decides with a strict lookahead
   (no exit, merge point, interpreter, call or memory access other than other [ebp+d'] slots in between). */
/* the block's instructions and the current one (lookahead: is a slot read again, overwritten later?) */
static const Insn *g_sw_blk;
static int g_sw_i, g_sw_n;
static int g_sw_dead_ok;            /* block allows it (no loop slots, hoisted loads or prefetch ring) */
static int g_sw_kill = -1, g_sw_avoid = -1;
static const int *g_sw_kinds, *g_sw_interp, *g_sw_traced;
static const uint8_t *g_sw_inner;
static int g_sw_stk = -1;
static unsigned long g_st_dead, g_st_dead_flush;

/* write a dirty entry to memory now (ebp is unchanged since its store: sw_killer); r0, or r12 if r0 holds a value */
static void sw_flush1(SwEntry *s)
{
    int t = g_sw_avoid == 0 ? 12 : 0;
    if (!s->valid || !s->dirty) return;
    s->dirty = 0;
    if (g_dead_log) fprintf(stderr, "rosetta: dead store [ebp%+d] written at %08x (kill %d, now %d)\n", (int)s->edisp,
                            g_sw_blk ? g_sw_blk[g_sw_i].eip : 0, s->kill, g_sw_i);
    e(LI(t, s->edisp));
    e(STWBRX(s->kind == 'g' ? R(s->reg) : s->reg, R(EBP), t));
    g_st_dead_flush++;
}

static void sw_flush_all(void) { int k; for (k = 0; k < SW_N; k++) sw_flush1(&g_sw[k]); }
static uint32_t g_sw_nr;                /* sequence number per entry: same number = same entry */
static int g_sw_next, g_sw_host_next, g_sw_host_from = 32;
static int g_sw_esp_ok, g_sw_ebp_ok;
static int32_t g_sw_esp, g_sw_ebp;
/* ebp unchanged since the block head while its relation to esp is unknown (a function body after the prologue): its slots
   [ebp+d] are offsets in a second name space, SW_NSB + d. A write in one name space may hit any slot of the other. */
#define SW_NSB 0x40000000
static int g_sw_ebp_own, g_sw_aebp_own;
static uint8_t g_sw_wm;                 /* registers the current instruction writes */
static unsigned long g_st_sw, g_st_sw_ret;

/* the entry of the current instruction's memory operand as it was before the instruction (sw_pre_insn drops what the
   instruction writes; a read-modify-write such as orl %eax,-0xa4(%ebp) reads the slot first) */
static SwEntry g_sw_pre;

static void sw_all_drop(void) { int k; sw_flush_all(); for (k = 0; k < SW_N; k++) g_sw[k].valid = 0; g_sw_pre.valid = 0; }

/* state at an inner jump; at the target only what holds on both paths remains */
typedef struct { SwEntry e[SW_N]; int esp_ok, ebp_ok, ebp_own; int32_t esp, ebp; } SwStatus;

static SwStatus g_inner_sw[MAX_INSNS];

static void sw_status_note(SwStatus *s)
{
    memcpy(s->e, g_sw, sizeof g_sw);
    s->esp_ok = g_sw_esp_ok; s->ebp_ok = g_sw_ebp_ok; s->esp = g_sw_esp; s->ebp = g_sw_ebp; s->ebp_own = g_sw_ebp_own;
}

static void sw_status_hit(const SwStatus *s)
{
    int k, m;
    for (k = 0; k < SW_N; k++) {
        if (!g_sw[k].valid) continue;
        for (m = 0; m < SW_N; m++)
            if (s->e[m].valid && s->e[m].nr == g_sw[k].nr && s->e[m].off == g_sw[k].off && s->e[m].kind == g_sw[k].kind &&
                s->e[m].reg == g_sw[k].reg && s->e[m].val == g_sw[k].val) break;
        if (m == SW_N) { sw_flush1(&g_sw[k]); g_sw[k].valid = 0; }   /* (before the label: falling-through path) */
    }
    if (!s->esp_ok || s->esp != g_sw_esp) g_sw_esp_ok = 0;
    if (!s->ebp_ok || s->ebp != g_sw_ebp) g_sw_ebp_ok = 0;
    if (!s->ebp_own) g_sw_ebp_own = 0;
}

/* at the block head: nothing known except esp = offset 0; host registers from host_from on are free */
static void sw_head(int host_from)
{
    int k;
    for (k = 0; k < SW_N; k++) g_sw[k].valid = g_sw[k].dirty = 0;    /* (nothing to write: a new block) */
    g_sw_pre.valid = 0;
    g_sw_skip = g_sw_skipped = 0;
    g_sw_esp_ok = g_sw_aesp_ok = 1; g_sw_esp = g_sw_aesp = 0;
    g_sw_ebp_ok = g_sw_aebp_ok = 0;
    g_sw_ebp_own = g_sw_aebp_own = !g_no_sw2;
    g_sw_host_from = host_from; g_sw_host_next = 0;
}

/* stack slot of the memory operand of b (state before the instruction) */
static int sw_slot(const Insn *b, int32_t *off)
{
    if (!b->mem || b->seg || b->index >= 0) return 0;
    if (b->base == ESP && g_sw_aesp_ok) { *off = g_sw_aesp + (int32_t)b->disp; return 1; }
    if (b->base == EBP && g_sw_aebp_ok) { *off = g_sw_aebp + (int32_t)b->disp; return 1; }
    if (b->base == EBP && g_sw_aebp_own && (int32_t)b->disp > -0x1000000 && (int32_t)b->disp < 0x1000000) {
        *off = SW_NSB + (int32_t)b->disp;
        return 1;
    }
    return 0;
}

static int sw_find(int32_t off)
{
    int k;
    if (g_no_sw) return -1;
    for (k = 0; k < SW_N; k++) if (g_sw[k].valid && g_sw[k].off == off) return k;
    return -1;
}

static void sw_write(int32_t off, int sz)       /* bytes [off, off + sz) written */
{
    int k, ns = off >= SW_NSB / 2;
    for (k = 0; k < SW_N; k++)
        if (g_sw[k].valid && ((g_sw[k].off < off + sz && off < g_sw[k].off + 4) || (g_sw[k].off >= SW_NSB / 2) != ns)) {
            /* the instruction that overwrites a dirty slot entirely (its kill): the left-out store stays dead */
            if (g_sw[k].dirty && !(g_sw[k].kill == g_sw_i && off <= g_sw[k].off && g_sw[k].off + 4 <= off + sz &&
                                   (g_sw[k].off >= SW_NSB / 2) == ns))
                sw_flush1(&g_sw[k]);
            g_sw[k].valid = g_sw[k].dirty = 0;
        }
}

static int sw_new(int32_t off)
{
    int k = g_sw_next, m;
    for (m = 0; m < SW_N; m++)
        if (g_sw[m].valid && g_sw[m].off < off + 4 && off < g_sw[m].off + 4) g_sw[m].valid = g_sw[m].dirty = 0;
    for (m = 0; m < SW_N && g_sw[k].valid; m++) k = (k + 1) % SW_N;    /* a free entry first, else the oldest in turn */
    if (m == SW_N) { k = g_sw_next; sw_flush1(&g_sw[k]); }
    g_sw_next = (k + 1) % SW_N;
    g_sw[k].off = off;
    g_sw[k].valid = 1;
    g_sw[k].dirty = 0;
    g_sw[k].reg = 0;
    g_sw[k].val = 0;
    g_sw[k].store = 0;
    g_sw[k].nr = ++g_sw_nr;
    return k;
}

static void sw_const(int32_t off, uint32_t w) { int k; if (g_no_sw) return; k = sw_new(off); g_sw[k].kind = 'k'; g_sw[k].val = w; }
static void sw_guest(int32_t off, int r) { int k; if (g_no_sw) return; k = sw_new(off); g_sw[k].kind = 'g'; g_sw[k].reg = (int8_t)r; }

/* guest register r goes onto the stack: copy in a free r28..r31 (survives changes of r), otherwise r itself */
/* value in host register q went to stack slot off: copy in a free r28..r31 (0: none free) */
/* the next copy register of r28..r31 in turn (entries that had their copy there are dropped); 0: none free */
static int sw_host_reg(void)
{
    int k, h;
    if (g_no_sw || g_sw_host_from > 31) return 0;
    h = g_sw_host_from + g_sw_host_next;
    g_sw_host_next = (g_sw_host_next + 1) % (32 - g_sw_host_from);
    for (k = 0; k < SW_N; k++)
        if (g_sw[k].valid && g_sw[k].kind == 'w' && g_sw[k].reg == h) { sw_flush1(&g_sw[k]); g_sw[k].valid = 0; }
    if (g_sw_pre.valid && g_sw_pre.kind == 'w' && g_sw_pre.reg == h) g_sw_pre.valid = 0;
    return h;
}

static int sw_reg_free(int h);

/* returns the entry + 1 (0: no copy register free) */
static int sw_host(int32_t off, int q)
{
    int k, h;
    if (!g_no_inplace && !g_no_sw && q >= g_sw_host_from && q <= 31 && sw_reg_free(q)) {
        h = q;                              /* computed in the old copy register (sw_copy_of): it stays the copy */
        if (g_sw_pre.valid && g_sw_pre.kind == 'w' && g_sw_pre.reg == h) g_sw_pre.valid = 0;
    } else {
        g_sw_avoid = q;                     /* (a dirty entry written on the way must not use q as its address) */
        h = sw_host_reg();
        g_sw_avoid = -1;
        if (!h) return 0;
        if (h != q) e(MR(h, q));
    }
    g_sw_avoid = q;
    k = sw_new(off);
    g_sw_avoid = -1;
    g_sw[k].kind = 'w';
    g_sw[k].reg = (int8_t)h;
    return k + 1;
}

static void sw_guest_save(int32_t off, int r)
{
    if (g_no_sw) return;
    if (!sw_host(off, R(r))) sw_guest(off, r);
}

/* is [ebp+d] read again in the block before it is overwritten or ebp changes (from the current instruction on, within
   n instructions)? */
static int sw_read_within(int32_t d, int n)
{
    int j;
    if (!g_sw_blk) return 1;
    for (j = g_sw_i; j < g_sw_n && j - g_sw_i < n; j++) {
        const Insn *y = &g_sw_blk[j];
        if (y->mem && y->base == EBP && y->index < 0 && !y->seg && (int32_t)y->disp == d)
            return !((y->op == 0x89 || y->op == 0xc7) && y->opsz == 4);     /* a plain store overwrites it unread */
        if (write_mask(y) >> EBP & 1) return 0;
    }
    return 0;
}

static int sw_read_later(int32_t d) { return sw_read_within(d, 1 << 30); }

static int slot_reg(const Insn *b, int sz);
static int g_sw_no_note;               /* the caller remembers the stored value itself (mov m,imm: a constant) */

/* after a 32-bit store of host register val to the memory operand of b: a stack slot holds val. A guest register
   that this instruction does not change is remembered as such; otherwise (a temporary, or xchg/xadd change it
   afterwards) a copy goes into a free r28..r31 right after the store. If the store was left out (g_sw_skipped), the
   entry is dirty; returns 0 if there is no entry (the caller stores after all). */
static int sw_store_note(const Insn *b, int val)
{
    int32_t off;
    int x, k = -1;
    if (g_no_sw || g_no_sw2 || g_sw_no_note || !sw_slot(b, &off) || slot_reg(b, 4) >= 0) return 0;
    for (x = 0; x < 8; x++)
        if (R(x) == val && !(g_sw_wm >> x & 1)) {
            k = sw_new(off);
            g_sw[k].kind = 'g'; g_sw[k].reg = (int8_t)x; g_sw[k].store = 1;
            break;
        }
    if (k < 0) {
        if ((k = sw_host(off, val)) <= 0) return 0;
        k--;
        g_sw[k].store = 1;
    }
    if (g_sw_skipped) {
        g_sw[k].dirty = 1; g_sw[k].kill = (int16_t)g_sw_kill; g_sw[k].edisp = (int32_t)b->disp;
        g_st_dead++;
    }
    return 1;
}

static void sw_fetch(int k, int t);

/* ---- dead stores (sw_killer) ---- */
static const uint8_t *g_sw_after;       /* flags read after each instruction of the block */

/* 32-bit write of the whole memory operand that is generated as one store (save_p + after_save) and reads the old
   value, if at all, through forwarding (rmw_val, g_sw_pre). j: index of the instruction (rotations go through the
   interpreter when CF/OF are read afterwards) */
static int sw_full_write(const Insn *y, int j)
{
    int op = y->op, r = y->reg;
    if (y->opsz != 4 || y->lock) return 0;
    if (op == 0x89 || (op == 0xc7 && r == 0)) return 1;
    if (op < 0x40 && (op & 7) == 1) { int a = op >> 3; return a == 0 || a == 1 || a == 4 || a == 5 || a == 6; }
    if (op == 0x81 || op == 0x83) return r == 0 || r == 1 || r == 4 || r == 5 || r == 6;
    if (op == 0xff) return r <= 1;
    if (op == 0xf7) return r == 2 || r == 3;
    if (op == 0xc1 && !(y->imm & 31)) return 0;     /* count 0: nothing is written */
    if (op == 0xc1 || op == 0xd1 || op == 0xd3) {
        if (r == 4 || r == 5 || r == 7) return 1;
        return r <= 1 && j >= 0 && g_sw_after && !(g_sw_after[j] & (FCF | FOF));
    }
    return 0;
}

/* 32-bit read of the memory operand through rm_val / alu_operands (forwarding answers it), nothing written there */
static int sw_reader(const Insn *y)
{
    int op = y->op, r = y->reg;
    if (y->opsz != 4) return 0;
    if (op == 0x8b || op == 0x85 || op == 0x69 || op == 0x6b || op == (OP_0F | 0xaf)) return 1;
    if (op >= (OP_0F | 0x40) && op <= (OP_0F | 0x4f)) return 1;
    if (op < 0x40 && ((op & 7) == 3 || op == 0x39)) { int a = op >> 3; return a != 2 && a != 3; }
    if (op == 0x81 || op == 0x83) return r == 7;
    if (op == 0xf7) return r == 0;
    return 0;
}

/* integer instruction that accesses memory only through its operand and calls no C code on a side path */
static int sw_plain_mem(const Insn *y)
{
    int op = y->op, r = y->reg;
    if (op >= 0x88 && op <= 0x8b) return 1;
    if (op == 0xc6 || op == 0xc7) return r == 0;
    if (op < 0x40 && (op & 7) < 4) { int a = op >> 3; return a != 2 && a != 3; }
    if (op >= 0x80 && op <= 0x83 && op != 0x82) return r != 2 && r != 3;
    if (op == 0x84 || op == 0x85 || op == 0x69 || op == 0x6b) return 1;
    if (op == (OP_0F | 0xaf) || op == (OP_0F | 0xb6) || op == (OP_0F | 0xb7) || op == (OP_0F | 0xbe) ||
        op == (OP_0F | 0xbf)) return 1;
    if (op >= (OP_0F | 0x40) && op <= (OP_0F | 0x4f)) return 1;
    if (op == 0xf6) return r == 0 || r == 2 || r == 3;
    if (op == 0xf7) return r == 0 || r == 2 || r == 3 || ((r == 4 || r == 5) && y->opsz == 4);
    if (op == 0xfe || op == 0xff) return r <= 1;
    if (op == 0xc1 || op == 0xd1 || op == 0xd3) return r <= 1 || r == 4 || r == 5 || r == 7;
    return 0;
}

/* integer instruction on registers only, without memory and without C code on a side path (no div) */
static int sw_plain_reg(const Insn *y)
{
    int op = y->op, r = y->reg;
    if (op < 0x40 && (op & 7) < 6) { int a = op >> 3; return a != 2 && a != 3; }
    if (op >= 0x40 && op <= 0x4f) return 1;
    if ((op >= 0x84 && op <= 0x8b) || (op >= 0x90 && op <= 0x99) || op == 0xa8 || op == 0xa9 ||
        (op >= 0xb0 && op <= 0xbf) || op == 0x69 || op == 0x6b) return 1;
    if (op >= 0x80 && op <= 0x83 && op != 0x82) return r != 2 && r != 3;
    if (op == 0xc1 || op == 0xd1 || op == 0xd3) return r <= 1 || r == 4 || r == 5 || r == 7;
    if (op == 0xf7) return r == 0 || r == 2 || r == 3 || ((r == 4 || r == 5) && y->opsz == 4);
    if (op == 0xff) return r <= 1;
    if (op >= (OP_0F | 0x40) && op <= (OP_0F | 0x4f)) return 1;
    if (op >= (OP_0F | 0x90) && op <= (OP_0F | 0x9f)) return 1;
    if (op >= (OP_0F | 0xc8) && op <= (OP_0F | 0xcf)) return 1;
    return op == (OP_0F | 0xaf) || op == (OP_0F | 0xb6) || op == (OP_0F | 0xb7) || op == (OP_0F | 0xbe) ||
           op == (OP_0F | 0xbf) || op == (OP_0F | 0xbc) || op == (OP_0F | 0xbd);
}

/* the current instruction's 32-bit store to [ebp+d]: index of the later instruction of the block that overwrites
   the whole slot with nothing in between that could see it or leave the block, otherwise -1 */
static int sw_killer(const Insn *b)
{
    int32_t d = (int32_t)b->disp;
    int j, writes = 0;
    if (!g_sw_dead_ok || !g_sw_blk || !g_sw_kinds || b->base != EBP || b->index >= 0 || b->seg || b->rep ||
        d < -32768 || d > 32767) return -1;
    /* short windows only: in a long one other slots take the copy registers, the left-out store is written late
       (sw_flush1) and a load of the slot shortly after waits for it (sha1: 2 % slower with windows up to 70) */
    for (j = g_sw_i + 1; j < g_sw_n && j <= g_sw_i + g_dead_window; j++) {
        const Insn *y = &g_sw_blk[j];
        int sz, l, s;
        int32_t dd;
        if (g_sw_kinds[j] != CONT || g_sw_interp[j] || g_sw_traced[j] || g_sw_inner[j] || j == g_sw_stk ||
            j == g_sw_stk + 1 || y->lock || y->rep || y->seg || (write_mask(y) >> EBP & 1)) return -1;
        if (y->op == 0x8d) continue;                    /* lea: no access */
        if (!y->mem) { if (!sw_plain_reg(y)) return -1; continue; }
        if (y->base != EBP || y->index >= 0) return -1;
        dd = (int32_t)y->disp;
        if (dd == d && sw_full_write(y, j)) return j;
        if (dd == d && sw_reader(y)) continue;
        if (!sw_plain_mem(y) || !mem_access(y, &sz, &l, &s)) return -1;
        if (s && ++writes > g_dead_writes) return -1;  /* another slot written: takes a copy register */
        if (dd + sz <= d || d + 4 <= dd) continue;
        return -1;
    }
    return -1;
}

/* a 32-bit stack slot read (operand of an ALU instruction or read-modify-write): the remembered value into t */
static int sw_load(const Insn *b, int t)
{
    int32_t off;
    int k;
    if (g_no_sw2 || !sw_slot(b, &off) || slot_reg(b, 4) >= 0) return 0;
    if ((k = sw_find(off)) >= 0) { sw_fetch(k, t); return 1; }
    if (g_sw_pre.valid && g_sw_pre.off == off) {      /* (sw_fetch with the saved entry) */
        if (g_sw_pre.kind == 'k') load_imm(t, g_sw_pre.val);
        else {
            int q = g_sw_pre.kind == 'g' ? R(g_sw_pre.reg) : g_sw_pre.reg;
            if (q != t) e(MR(t, q));
        }
        g_st_sw++;
        return 1;
    }
    return 0;
}

/* does the 32-bit stack slot of b's memory operand come from forwarding (sw_load hits)? */
static int sw_fwd_ok(const Insn *b)
{
    int32_t off;
    if (g_no_sw2 || g_no_inplace || !sw_slot(b, &off) || slot_reg(b, 4) >= 0) return 0;
    return sw_find(off) >= 0 || (g_sw_pre.valid && g_sw_pre.off == off);
}

/* copy register (r28..r31) of no entry? */
static int sw_reg_free(int h)
{
    int k;
    for (k = 0; k < SW_N; k++) if (g_sw[k].valid && g_sw[k].kind == 'w' && g_sw[k].reg == h) return 0;
    return 1;
}

/* the 32-bit stack slot of b's memory operand has its value in a copy register: that register (0: no copy).
   own: the instruction overwrites the slot with a result computed from it, so the caller computes in the register
   itself (only if no other entry refers to it; the store makes it the slot's copy again, sw_host). Otherwise the
   caller only reads the register (no mr into a temporary). */
static int sw_copy_of(const Insn *b, int own)
{
    int32_t off;
    int k, h;
    if (g_no_sw || g_no_sw2 || g_no_inplace || !sw_slot(b, &off) || slot_reg(b, 4) >= 0) return 0;
    if ((k = sw_find(off)) >= 0) {
        if (g_sw[k].kind != 'w' || own) return 0;   /* (a written slot is no longer in the table: g_sw_pre) */
        h = g_sw[k].reg;
    } else if (g_sw_pre.valid && g_sw_pre.off == off && g_sw_pre.kind == 'w') {
        h = g_sw_pre.reg;
        if (own && !sw_reg_free(h)) return 0;
    } else return 0;
    if (h < g_sw_host_from || h > 31) return 0;
    g_st_sw++;
    return h;
}

/* 8- or 16-bit read inside a remembered 32-bit stack slot (AES: shrl $0x10,0xdc(%ebp); movzbl 0xdc(%ebp),%eax - the
   G4 would wait for the store): the bytes from the slot's value (little endian: byte k is bits 8k..8k+7),
   zero-extended into t */
static int sw_load_narrow(const Insn *b, int sz, int t)
{
    int32_t off;
    int k, sh;
    if (g_no_sw || g_no_sw2 || g_no_narrowfw || !sw_slot(b, &off) || slot_reg(b, 4) >= 0) return 0;
    for (k = 0; k < SW_N; k++) {
        const SwEntry *s = &g_sw[k];
        if (!s->valid || off < s->off || off + sz > s->off + 4) continue;   /* (name spaces are SW_NSB apart) */
        sh = 8 * (off - s->off);
        if (s->kind == 'k') load_imm(t, (s->val >> sh) & (sz == 1 ? 0xffu : 0xffffu));
        else e(RLWINM(t, s->kind == 'g' ? R(s->reg) : s->reg, (32 - sh) & 31, sz == 1 ? 24 : 16, 31));
        g_st_sw++;
        return 1;
    }
    return 0;
}

/* value of slot k into host register t */
static void sw_fetch(int k, int t)
{
    if (g_sw[k].kind == 'k') load_imm(t, g_sw[k].val);
    else {
        int q = g_sw[k].kind == 'g' ? R(g_sw[k].reg) : g_sw[k].reg;
        if (q != t) e(MR(t, q));
    }
    g_st_sw++;
}

/* before every instruction: remember the old esp/ebp state, discard what is invalid, determine esp/ebp after it */
static void sw_pre_insn(const Insn *b, int interp, int verf)
{
    int r, k, op = b->op, esp_done = 0, ebp_done = 0, used = 0, guest = 0;
    int32_t off;
    g_sw_aesp_ok = g_sw_esp_ok; g_sw_aesp = g_sw_esp;
    g_sw_aebp_ok = g_sw_ebp_ok; g_sw_aebp = g_sw_ebp;
    g_sw_aebp_own = g_sw_ebp_own;
    g_sw_wm = interp ? 0xff : write_mask(b);
    if (interp || (g_sw_wm >> EBP & 1)) g_sw_ebp_own = 0;   /* (state after the instruction) */
    g_sw_pre.valid = 0;
    g_sw_skip = g_sw_skipped = 0;
    g_sw_kill = -1;
    for (k = 0; k < SW_N; k++)              /* a left-out store outside its window (sw_killer): write it now */
        if (g_sw[k].valid && g_sw[k].dirty && (interp || verf || g_sw_i > g_sw[k].kill)) sw_flush1(&g_sw[k]);
    if (verf == 5) return;                  /* direct math: no memory, esp unchanged (c_call flushes afterwards) */
    for (k = 0; k < SW_N; k++) if (g_sw[k].valid) { used = 1; if (g_sw[k].kind == 'g') guest = 1; }
    if (used && !interp && !g_no_sw2 && b->mem && sw_slot(b, &off) && (k = sw_find(off)) >= 0) g_sw_pre = g_sw[k];
    if (used) {                           /* only then is the check worth it (translation stays fast) */
        /* memory: what the instruction can write is invalid */
        if (interp) sw_all_drop();
        else if ((op >= 0x50 && op <= 0x57) || op == 0x68 || op == 0x6a || op == 0xe8 || op == 0x9c || op == 0x60 ||
                 (op == 0xff && (b->reg == 2 || b->reg == 6))) {
            if (g_sw_aesp_ok) sw_write(g_sw_aesp - 32, 32); else sw_all_drop();   /* push (pusha up to 32 bytes) */
        } else if (!no_memwrite(b)) {
            /* actual size (16 would also have discarded the entry for [esp+4] on mov [esp],eax) */
            if (sw_slot(b, &off)) sw_write(off, write_size(b)); else sw_all_drop();
        }
        /* registers: entries with a changed guest register */
        if (guest) {                       /* a stored value moves to a copy register first (before this instruction) */
            uint8_t wm = g_sw_wm;
            for (k = 0; k < SW_N; k++)
                if (g_sw[k].valid && g_sw[k].kind == 'g' && (wm >> g_sw[k].reg & 1)) {
                    /* a loaded one too if the block reads the slot again (gcc reloads pointers from the frame:
                       movl -0x1fc(%ebp),%eax; movzbl (%eax),%eax, again for every byte) */
                    int keep = g_sw[k].dirty ||
                               (g_sw[k].store ? g_sw[k].off < SW_NSB / 2 || sw_read_later(g_sw[k].off - SW_NSB)
                                              : !g_no_loadcopy && g_sw[k].off >= SW_NSB / 2 &&
                                                sw_read_within(g_sw[k].off - SW_NSB, g_loadcopy_dist));
                    int h = keep && !interp ? sw_host_reg() : 0;
                    if (h) { e(MR(h, R(g_sw[k].reg))); g_sw[k].kind = 'w'; g_sw[k].reg = (int8_t)h; }
                    else { sw_flush1(&g_sw[k]); g_sw[k].valid = 0; }
                }
        }
    }
    (void)r;
    /* esp/ebp afterwards */
    if (!interp) {
        if (op == 0xe8 && verf == 7) {      /* direct stub: return address pushed and removed again */
            g_sw_esp = g_sw_aesp; esp_done = 1;
        } else if ((op >= 0x50 && op <= 0x57) || op == 0x68 || op == 0x6a || (op == 0xff && b->reg == 6) ||
            (op == 0xe8 && verf == 2) || (op == 0xff && b->reg == 2 && (verf == 4 || verf == 8))) {
            g_sw_esp = g_sw_aesp - 4; esp_done = 1;
        } else if (op >= 0x58 && op <= 0x5f && op != 0x5c) {
            g_sw_esp = g_sw_aesp + 4; esp_done = 1;
        } else if ((op == 0xc3 || op == 0xc2) && verf == 3) {
            g_sw_esp = g_sw_aesp + 4 + (op == 0xc2 ? (int32_t)(b->imm & 0xffff) : 0); esp_done = 1;
        } else if ((op == 0x81 || op == 0x83) && !b->mem && b->opsz == 4 && (b->reg == 0 || b->reg == 5) &&
                   (b->rm == ESP || b->rm == EBP)) {
            int32_t d = b->reg == 0 ? (int32_t)b->imm : -(int32_t)b->imm;
            if (b->rm == ESP) { g_sw_esp = g_sw_aesp + d; esp_done = 1; } else { g_sw_ebp = g_sw_aebp + d; ebp_done = 1; }
        } else if ((op == 0x89 || op == 0x8b) && !b->mem && b->opsz == 4) {
            int z = op == 0x89 ? b->rm : b->reg, q = op == 0x89 ? b->reg : b->rm;
            if (z == EBP && q == ESP) { g_sw_ebp_ok = g_sw_aesp_ok; g_sw_ebp = g_sw_aesp; ebp_done = 1; }
            if (z == ESP && q == EBP) { g_sw_esp_ok = g_sw_aebp_ok; g_sw_esp = g_sw_aebp; esp_done = 1; }
        } else if (op == 0xc9) {
            g_sw_esp_ok = g_sw_aebp_ok; g_sw_esp = g_sw_aebp + 4; esp_done = 1;
        } else if (op == 0x8d && !b->seg && b->index < 0 && (b->reg == ESP || b->reg == EBP) &&
                   ((b->base == ESP && g_sw_aesp_ok) || (b->base == EBP && g_sw_aebp_ok))) {
            int32_t v = (b->base == ESP ? g_sw_aesp : g_sw_aebp) + (int32_t)b->disp;
            if (b->reg == ESP) { g_sw_esp_ok = 1; g_sw_esp = v; esp_done = 1; }
            else { g_sw_ebp_ok = 1; g_sw_ebp = v; ebp_done = 1; }
        }
    }
    if (!esp_done || !ebp_done) {
        uint8_t wm = interp ? 0xff : write_mask(b);
        if (!esp_done && (wm >> ESP & 1)) g_sw_esp_ok = 0;
        if (!ebp_done && (wm >> EBP & 1)) g_sw_ebp_ok = 0;
    }
    /* a store of this instruction that a later one overwrites unseen: leave it out (save_p, sw_store_note) */
    if (g_sw_dead_ok && !interp && !verf && op != 0xc7 && b->mem && sw_full_write(b, g_sw_i) &&
        sw_slot(b, &off) && (g_sw_kill = sw_killer(b)) >= 0)
        g_sw_skip = 1;
}

/* ---------- SSE floating point (scalar) ----------
 * The low element of xmm n lives in f14+n within the block as needed: as a double ('D', *sd) or as a float ('F',
 * *ss; held as a double in the FPR, computed with the s instructions). Cpu.xmm (raw Intel bytes) is current again
 * only after write-back: before every exit (side exits in their exit stub), before the interpreter and before
 * AltiVec instructions. Byte order through lwbrx/stwbrx and the scratch area in the stack frame (the G4 cannot load
 * floating-point values byte-swapped). */
/* high: high half in memory still has to become 0; pending: value lies byte-swapped in the scratch area (offset),
   the lfd comes only at first use (the G4 does not forward stw to lfd: store early, load late) */
/* High half (packed doubles): bytes 8..15 of xmm n as a double in f24+n (h), memory stale (hs), value still in
   the store (hhome, like pending). Only in blocks without x87 (g_xfh_on; there f24..f31 belong to the x87 stack),
   otherwise packed high halves go through Cpu.xmm. high and h are mutually exclusive. */
typedef struct { char kind, dirty, high; short pending; char h, hs; short hhome; } XfState;
static XfState g_xf[8];
static int g_xfh_on;
static uint8_t g_fp_ph;                 /* loop registers whose high half is also loaded before the head */
static unsigned long g_st_pd;
#define XFH(n) (24 + (n))
static unsigned long g_st_fp, g_st_fp_loops;
/* loop block: P = loaded before the head and kept across the backward jump, T = dead at the head (full load first) */
static uint8_t g_fp_p, g_fp_t;
static char g_fp_kind[8];

/* zero the high half of xmm n in memory (after movsd/movss from memory) */
static void xf_zero_upper(int n, char kind)
{
    e(LI(0, 0));
    if (kind == 'F') e(STW(0, xmm_off(n) + 4, RCPU));
    e(STW(0, xmm_off(n) + 8, RCPU));
    e(STW(0, xmm_off(n) + 12, RCPU));
}

/* Host form at block boundaries (Cpu.xmm_h/xmm_hf): exits store the low value with stfd/stfs (instead of ~10
   instructions with a load-after-store stall into Intel format), the next block fetches it with lfd/lfs. Intel
   format is produced only when someone needs it: in the block xf_back/xf_forget (for registers that are not cached,
   by checking the tag byte, xmm_mat_code), in C xmm_intel(). g_xf_host: write-back may use the host form; g_xm_ok:
   registers whose tag byte is certainly 0 here. */
static int g_xf_host;
static uint8_t g_xm_ok;
static unsigned long g_st_xhost;

static int xh_off(int n) { return (int)OFF(xmm_h) + 8 * n; }
static int xhf_off(int n) { return (int)OFF(xmm_hf) + n; }

/* xmm n has been written entirely in Intel format: tag byte 0 */
static void xmm_new_intel(int n)
{
    if (g_no_xhost || (g_xm_ok >> n & 1)) return;
    e(LI(0, 0)); e(STB(0, xhf_off(n), RCPU));
    g_xm_ok |= (uint8_t)(1u << n);
}

/* run time: if xmm n is in host form, convert it back to Intel format (tag byte 0 afterwards). r0, r11, r12, cr1 */
static void xmm_mat_code(int n)
{
    uint32_t *cont, *fl, *end;
    int h = xh_off(n), x = xmm_off(n);
    if (g_no_xhost || (g_xm_ok >> n & 1)) return;
    e(LBZ(0, xhf_off(n), RCPU));
    e(CMPWI(1, 0, 0));
    cont = p; e(BC_(BO_TRUE, 4 + CR_EQ, 0));
    e(CMPWI(1, 0, 'F'));
    fl = p; e(BC_(BO_TRUE, 4 + CR_EQ, 0));
    e(LWZ(11, h, RCPU)); e(LWZ(12, h + 4, RCPU));           /* double: host words high, low */
    e(LI(0, x)); e(STWBRX(12, RCPU, 0)); e(LI(0, x + 4)); e(STWBRX(11, RCPU, 0));
    end = p; e(B_(0));
    bc_set(fl, p);
    e(LWZ(11, h, RCPU)); e(LI(0, x)); e(STWBRX(11, RCPU, 0));   /* float */
    *end = B_((uint32_t)(uintptr_t)p - (uint32_t)(uintptr_t)end);
    e(LI(0, 0)); e(STB(0, xhf_off(n), RCPU));
    bc_set(cont, p);
    g_xm_ok |= (uint8_t)(1u << n);
}

/* write-back code for xmm n (kind art), without changing the state */
static void xf_back_code(int n, char kind, int high)
{
    if (high) xf_zero_upper(n, kind);
    if (g_xf_host && !g_no_xhost) {
        e(kind == 'D' ? STFD(XF(n), xh_off(n), RCPU) : STFS(XF(n), xh_off(n), RCPU));
        e(LI(0, kind)); e(STB(0, xhf_off(n), RCPU));
        g_st_xhost++;
        return;
    }
    if (kind == 'D') {
        e(STFD(XF(n), TMP(0), 1));
        e(LWZ(3, TMP(0), 1)); e(LWZ(4, TMP(0) + 4, 1));
        e(ADDI(12, RCPU, xmm_off(n)));
        e(STWBRX(4, 0, 12)); e(LI(0, 4)); e(STWBRX(3, 12, 0));
    } else {
        e(STFS(XF(n), TMP(0), 1));
        e(LWZ(3, TMP(0), 1));
        e(LI(0, xmm_off(n))); e(STWBRX(3, RCPU, 0));
    }
    if (!g_no_xhost && !(g_xm_ok >> n & 1)) { e(LI(0, 0)); e(STB(0, xhf_off(n), RCPU)); g_xm_ok |= (uint8_t)(1u << n); }
}

/* execute the pending lfd now */
static void xf_done(int n)
{
    if (!g_xf[n].pending) return;
    e(g_xf[n].kind == 'D' ? LFD(XF(n), g_xf[n].pending, RCPU) : LFS(XF(n), g_xf[n].pending, RCPU));
    g_xf[n].pending = 0;
}

static void xfh_done(int n)
{
    if (!g_xf[n].hhome) return;
    e(LFD(XFH(n), g_xf[n].hhome, RCPU));
    g_xf[n].hhome = 0;
}

static void xf_all_done(void) { int n; for (n = 0; n < 8; n++) { xf_done(n); xfh_done(n); } }

/* storage slot z (16 bytes) is reused: load values pending there first */
static void slot_done(int z)
{
    int n;
    for (n = 0; n < 8; n++) {
        if (g_xf[n].pending >= z && g_xf[n].pending < z + 16) xf_done(n);
        if (g_xf[n].hhome >= z && g_xf[n].hhome < z + 16) xfh_done(n);
    }
}

/* next slot of the floating-point store in Cpu (relative to RCPU); if a pending value is still there, load it first */
static int g_vz;
static int vz_new(void)
{
    int z;
    g_vz = (g_vz + 1) & 7;
    z = (int)OFF(fp_store) + 16 * g_vz;
    slot_done(z);
    fw_slot_drop(z);
    return z;
}

/* double/float at address (register a, Intel byte order) byte-swapped via AltiVec to storage slot z: both quadwords
   that hold the value (also across a 16-byte boundary), swap pattern by a & 15 */
static void vswap_put(int a, char kind, int z)
{
    e(LI(0, kind == 'D' ? 7 : 3));
    e(LVX(1, a, 0));                        /* quadword with the last byte */
    e(LVX(0, 0, a));                        /* quadword with the first byte */
    e(RLWINM(11, a, 4, 24, 27));            /* (a & 15) * 16 */
    e(ADDI(11, 11, (int)(kind == 'D' ? OFF(vswap8) : OFF(vswap4))));
    e(LVX(2, RCPU, 11));
    e(VPERM(3, 0, 1, 2));
    e(LI(0, z));
    e(STVX(3, RCPU, 0));
}

/* next slot of the scratch area; if a pending value is still there, it is loaded first */
static int tmp_new(void)
{
    int n, z;
    g_tmp = g_tmp >= 12 || g_tmp < 2 ? 2 : g_tmp + 1;
    z = TMP(g_tmp);
    for (n = 0; n < 8; n++) if (g_xf[n].pending == z) xf_done(n);
    return z;
}

/* pending value from the scratch area (byte-swapped) to memory without the FPU (register a, offset 0) */
static void tmp_save(int z, char kind, int a)
{
    if (kind == 'D') { e(STWBRX(4, 0, a)); e(LI(0, 4)); e(STWBRX(3, a, 0)); }
    else e(STWBRX(3, 0, a));
    (void)z;
}

/* high half of xmm n from FPR f (z = 0) or pending from storage slot z into Intel format (bytes 8..15) */
static void xfh_back_code(int n, int f, int z)
{
    if (z) { e(LWZ(3, z, RCPU)); e(LWZ(4, z + 4, RCPU)); }
    else { e(STFD(f, TMP(0), 1)); e(LWZ(3, TMP(0), 1)); e(LWZ(4, TMP(0) + 4, 1)); }
    e(ADDI(12, RCPU, xmm_off(n) + 8));
    e(STWBRX(4, 0, 12)); e(LI(0, 4)); e(STWBRX(3, 12, 0));
}

static void xfh_back(int n)
{
    if (!g_xf[n].h || !g_xf[n].hs) return;
    xfh_back_code(n, XFH(n), g_xf[n].hhome);
    g_xf[n].hs = 0;
}

static void xfh_drop(int n) { g_xf[n].h = 0; g_xf[n].hs = 0; g_xf[n].hhome = 0; }

/* high half of xmm n (Intel, Cpu.xmm) into FPR f */
static void xfh_load_code(int n, int f)
{
    int z = vz_new();
    e(LI(0, xmm_off(n))); e(LVX(0, RCPU, 0));
    e(LI(11, (int)OFF(vswapp))); e(LVX(2, RCPU, 11));
    e(VPERM(3, 0, 0, 2));
    e(LI(0, z)); e(STVX(3, RCPU, 0));
    e(LFD(f, z + 8, RCPU));
}

/* make xmm n current in Cpu.xmm (in the block: Intel format, also for registers that are not cached; at
   exits with g_xf_host: host form is enough) */
static void xf_back_bottom(int n);
static void xf_back(int n) { xf_back_bottom(n); xfh_back(n); }
static void xf_back_bottom(int n)
{
    if (!g_xf[n].kind) { if (!g_xf_host) xmm_mat_code(n); return; }
    if (g_xf[n].pending && g_xf[n].dirty) {   /* pending stays: the FPR is not loaded yet */
        int z = g_xf[n].pending;
        if (g_xf[n].high) xf_zero_upper(n, g_xf[n].kind);
        e(LWZ(3, z, RCPU));
        if (g_xf[n].kind == 'D') { e(LWZ(4, z + 4, RCPU)); e(ADDI(12, RCPU, xmm_off(n))); tmp_save(z, 'D', 12); }
        else { e(ADDI(12, RCPU, xmm_off(n))); tmp_save(z, 'F', 12); }
        if (!g_no_xhost && !(g_xm_ok >> n & 1)) { e(LI(0, 0)); e(STB(0, xhf_off(n), RCPU)); if (!g_xf_host) g_xm_ok |= (uint8_t)(1u << n); }
        g_xf[n].dirty = 0;
        g_xf[n].high = 0;
        return;
    }
    xf_done(n);
    if (!g_xf[n].dirty) return;
    xf_back_code(n, g_xf[n].kind, g_xf[n].high);
    g_xf[n].dirty = 0;
    g_xf[n].high = 0;
}

/* at exits (the block ends afterwards): host form */
static void xf_all_back(void) { int n, prev = g_xf_host; g_xf_host = 1; for (n = 0; n < 8; n++) xf_back(n); g_xf_host = prev; }
static void xf_forget(int n) { xf_back(n); g_xf[n].kind = 0; g_xf[n].high = 0; g_xf[n].pending = 0; xfh_drop(n); }
static void xf_drop(int n) { g_xf[n].kind = 0; g_xf[n].high = 0; g_xf[n].pending = 0; xfh_drop(n); }  /* is replaced entirely */
/* write everything back (host form) and forget: before C code that itself calls xmm_intel(), and at merge points */
static void xf_all_forget(void)
{
    int n, prev = g_xf_host;
    g_xf_host = 1;
    for (n = 0; n < 8; n++) { xf_back(n); xf_drop(n); }
    g_xf_host = prev;
    g_xm_ok = 0;
}

static void xf_load_code(int n, char kind)
{
    int z = vz_new();                       /* Cpu.xmm is 16-byte aligned: one quadword, swap pattern for offset 0 */
    uint32_t *slow = 0, *done = 0;
    if (!g_no_xhost && !(g_xm_ok >> n & 1)) {     /* stored in host form (tag byte = art): lfd/lfs, otherwise Intel */
        e(LBZ(0, xhf_off(n), RCPU));
        e(CMPWI(1, 0, kind));
        slow = p; e(BC_(BO_FALSE, 4 + CR_EQ, 0));
        e(kind == 'D' ? LFD(XF(n), xh_off(n), RCPU) : LFS(XF(n), xh_off(n), RCPU));
        done = p; e(B_(0));
        bc_set(slow, p);
        xmm_mat_code(n);                    /* other kind in host form: to Intel format first */
        g_xm_ok &= (uint8_t)~(1u << n);     /* (certainly 0 only on this path) */
    }
    e(LI(0, xmm_off(n))); e(LVX(0, RCPU, 0));
    e(LI(11, (int)(kind == 'D' ? OFF(vswap8) : OFF(vswap4)))); e(LVX(2, RCPU, 11));
    e(VPERM(3, 0, 0, 2));
    e(LI(0, z)); e(STVX(3, RCPU, 0));
    e(kind == 'D' ? LFD(XF(n), z, RCPU) : LFS(XF(n), z, RCPU));
    if (done) *done = B_((uint32_t)(uintptr_t)p - (uint32_t)(uintptr_t)done);
}

static void xf_load(int n, char kind)
{
    int host = !g_no_xhost && !(g_xm_ok >> n & 1);
    xf_load_code(n, kind);
    g_xf[n].kind = kind;
    g_xf[n].dirty = host;               /* loaded from the host form: Intel format stale, write back before Intel readers */
    g_xf[n].high = 0;
    g_xf[n].pending = 0;
}

/* write back state z (everything dirty) without changing g_xf (side exits: host form) */
static void xf_state_back(const XfState *z)
{
    int n, prev = g_xf_host;
    g_xf_host = 1;
    for (n = 0; n < 8; n++) {
        if (z[n].kind && z[n].dirty) xf_back_code(n, z[n].kind, z[n].high);
        if (z[n].h && z[n].hs) xfh_back_code(n, XFH(n), z[n].hhome);
    }
    g_xf_host = prev;
}

/* Backward jump to the loop head: P registers loaded as their kind and high half in memory valid, T registers dead,
   all others written back (at the head only P is in the cache; host form is enough, the head reloads) */
static void xf_sync(const XfState *z)
{
    int n;
    g_xf_host++;
    for (n = 0; n < 8; n++) {
        if (g_fp_t & (1 << n)) continue;
        if (g_fp_p & (1 << n)) {
            int ph = g_fp_ph >> n & 1;
            if (z[n].kind == g_fp_kind[n]) { if (z[n].high && !ph) xf_zero_upper(n, z[n].kind); }
            else {
                if (z[n].kind && z[n].dirty) xf_back_code(n, z[n].kind, z[n].high);
                xf_load_code(n, g_fp_kind[n]);
            }
            if (ph) {                       /* high half in the FPR at the head */
                if (z[n].h) { if (z[n].hhome) e(LFD(XFH(n), z[n].hhome, RCPU)); }
                else if (z[n].high) e(FSUB(XFH(n), F_MAGIC, F_MAGIC));
                else xfh_load_code(n, XFH(n));
            } else if (z[n].h && z[n].hs) xfh_back_code(n, XFH(n), z[n].hhome);
            continue;
        }
        if (z[n].kind && z[n].dirty) xf_back_code(n, z[n].kind, z[n].high);
        if (z[n].h && z[n].hs) xfh_back_code(n, XFH(n), z[n].hhome);
    }
    g_xf_host--;
}

/* FPR with the low element of xmm n as art (not cached: the load checks the host form itself);
   the high half stays as it is */
static int xf_fetch(int n, char kind)
{
    if (g_xf[n].kind != kind) {
        if (g_xf[n].kind) xf_back_bottom(n);
        g_xf[n].kind = 0; g_xf[n].high = 0; g_xf[n].pending = 0;
        xf_load(n, kind);
    } else xf_done(n);
    return XF(n);
}

/* resolve high, because the high half is about to be set differently (for float: zero bytes 4..7 now) */
static void high_drop(int n)
{
    if (!g_xf[n].high) return;
    if (g_xf[n].kind == 'F') { e(LI(0, 0)); e(STW(0, xmm_off(n) + 4, RCPU)); }
    g_xf[n].high = 0;
}

/* FPR with the high half of xmm n (without cache: into FPR t) */
static int xfh_fetch(int n, int t)
{
    if (g_xf[n].h) { xfh_done(n); return XFH(n); }
    if (g_xf[n].high) {                     /* the pending 0 */
        if (!g_xfh_on) { e(FSUB(t, F_MAGIC, F_MAGIC)); return t; }
        high_drop(n);
        e(FSUB(XFH(n), F_MAGIC, F_MAGIC));
        g_xf[n].h = 1; g_xf[n].hs = 1; g_xf[n].hhome = 0;
        return XFH(n);
    }
    if (!g_xfh_on) { xfh_load_code(n, t); return t; }
    xfh_load_code(n, XFH(n));
    g_xf[n].h = 1; g_xf[n].hs = 0; g_xf[n].hhome = 0;
    return XFH(n);
}

/* high half of xmm n is now the value in FPR f */
static void xfh_set(int n, int f)
{
    high_drop(n);
    if (g_xfh_on) {
        if (f != XFH(n)) e(FMR(XFH(n), f));
        g_xf[n].h = 1; g_xf[n].hs = 1; g_xf[n].hhome = 0;
        return;
    }
    xfh_back_code(n, f, 0);
}

/* high half of xmm n is now the value that lies byte-swapped in storage slot z (lfd only on use) */
static void xfh_set_slot(int n, int z)
{
    high_drop(n);
    if (g_xfh_on) { g_xf[n].h = 1; g_xf[n].hs = 1; g_xf[n].hhome = (short)z; return; }
    xfh_back_code(n, 0, z);
}

/* xmm n is about to get a new low element of kind art */
static void xf_set_pre(int n, char kind)
{
    /* bytes 4..7 of the old value stay: they must be valid in Intel format (also from the host form) */
    if (kind == 'F' && (g_xf[n].kind == 'D' || !g_xf[n].kind)) xf_back(n);
}

static void xf_set(int n, char kind) { g_xf[n].kind = kind; g_xf[n].dirty = 1; g_xf[n].pending = 0; }

/* memory operand (Intel byte order) byte-swapped into the store; returns the offset to RCPU (lfd/lfs later) */
/* hoisted (fp_hoist): storage slot of the current instruction, already swapped (0: none) */
static int g_pre_z;
static char g_pre_kind;

static int m_pre(const Insn *b, char kind)
{
    int a, z;
    if (g_pre_z && g_pre_kind == kind) { z = g_pre_z; g_pre_z = 0; return z; }
    a = ea_in(b);
    z = vz_new();
    vswap_put(a, kind, z);
    return z;
}

/* ---------- Hoisting floating-point loads ----------
 * Every double/float from memory needs ~31 cycles (AltiVec swap, store, lfd). The Intel code mostly needs the value
 * right away. So the swap is issued up to 12 instructions earlier: from where the address registers are fixed,
 * with no side exit, merge point, call or possible overwrite in between. At the instruction itself only the lfd
 * from the slot remains (pool Cpu.fp_pre, 32 slots round-robin). */
static unsigned long g_st_fpahead;

static int fp_hoist(const Insn *b, const int *kinds, const int *traced, const int *interp,
                        const uint32_t *cval, const int *stream, const uint8_t *inner_target, int n, int *pre_pos)
{
    int i, k, m, cnt = 0;
    for (i = 0; i < n; i++) pre_pos[i] = -1;
    for (i = 1; i < n; i++) {
        const Insn *x = &b[i];
        int sz = fp_load_size(x), good;
        uint8_t addr = 0;
        if ((sz != 4 && sz != 8 && !(sz == 16 && pd_slot_op(x))) || cval[i] || stream[i] || x->seg || interp[i] ||
            inner_target[i]) continue;
        if (x->base >= 0) addr |= (uint8_t)(1u << x->base);
        if (x->index >= 0) addr |= (uint8_t)(1u << x->index);
        /* already stored to the same form in the block: forwarding supplies it */
        for (m = 0; m < i; m++) {
            int g2, l2, s2;
            if (mem_access(&b[m], &g2, &l2, &s2) && s2 && b[m].mem && b[m].base == x->base && b[m].index == x->index &&
                b[m].disp == x->disp) break;
        }
        if (m < i) continue;
        good = i;
        for (k = i - 1; k >= 0 && i - k <= 12; k--) {
            const Insn *y = &b[k];
            int g2, l2, s2;
            if (inner_target[k + 1] && k + 1 <= i) break;    /* merge point between issue and use */
            if (interp[k] || (traced[k] && traced[k] != 1)) break;
            if (kinds[k] != CONT && !(kinds[k] == JUMP && traced[k])) break;
            if (write_mask(y) & addr) break;
            if (is_push(y)) break;
            if (mem_access(y, &g2, &l2, &s2) && s2) {
                if (!y->mem || y->seg || y->base != x->base || y->index != x->index || y->base < 0 ||
                    (y->index >= 0 && y->scale != x->scale) ||
                    !((int32_t)y->disp + g2 <= (int32_t)x->disp || (int32_t)x->disp + sz <= (int32_t)y->disp)) break;
            }
            good = k;
        }
        if (good < i) { pre_pos[i] = good; cnt++; }
    }
    return cnt;
}

/* issue the hoisted load of x into slot s (address registers already have their value at use here) */
static void fp_pre_kick(const Insn *x, int s)
{
    int sz = fp_load_size(x), z = (int)OFF(fp_pre) + 16 * s, a;
    slot_done(z);
    fw_slot_drop(z);
    if (sz == 16) { pd_swap(x, z); g_st_fpahead++; return; }
    a = ea_in(x);
    vswap_put(a, sz == 8 ? 'D' : 'F', z);
    g_st_fpahead++;
}

/* memory operand (Intel byte order) into FPR f */
static void m_load(int f, const Insn *b, char kind)
{
    int z;
    if (g_vl_cur) { vl_load(f, b); return; }   /* array: from the ring, stored iterations earlier */
    if (g_kaddr && pool_load(f, kind)) return;   /* constant: without detour */
    if ((z = fw_find(b, kind == 'D' ? 8 : 4)) >= 0) { if (z != f) e(FMR(f, z)); return; }   /* stored in the block */
    z = m_pre(b, kind);
    e(kind == 'D' ? LFD(f, z, RCPU) : LFS(f, z, RCPU));
}

static void m_save(int f, const Insn *b, char kind)
{
    int a;
    if (kind == 'D') { e(STFD(f, TMP(1), 1)); e(LWZ(3, TMP(1), 1)); e(LWZ(4, TMP(1) + 4, 1)); }   /* slot 1: fixed, never pending */
    else { e(STFS(f, TMP(1), 1)); e(LWZ(3, TMP(1), 1)); }
    a = ea_in(b);
    if (kind == 'D') { e(STWBRX(4, 0, a)); e(LI(0, 4)); e(STWBRX(3, a, 0)); }
    else e(STWBRX(3, 0, a));
    after_save(b, kind == 'D' ? 8 : 4, 0, a, 3);
}

/* scalar SSE instructions and copies the translator does itself */
static int fp_can(const Insn *b)
{
    int op = b->op, sk = (b->rep == 0xf2 || b->rep == 0xf3) && !b->p66;
    if (op < OP_0F || op > (OP_0F | 0xff)) return 0;
    switch (op & 0xff) {
    case 0x10: case 0x11: case 0x58: case 0x59: case 0x5c: case 0x5e: case 0x51: case 0x5a: case 0x2a: case 0x2c: case 0x2d:
        return sk;
    case 0x28: case 0x29: case 0x54: case 0x55: case 0x56: case 0x57: case 0x2e: case 0x2f:
        return !b->rep;                     /* movaps/movapd, and/andn/or/xor ps/pd, (u)comiss/sd */
    }
    return 0;
}

static int is_fp_cmp(const Insn *b)
{
    return (b->op == (OP_0F | 0x2e) || b->op == (OP_0F | 0x2f)) && !b->rep;
}

static void fp_calc(int op, char kind, int d, int a, int c)
{
    if (kind == 'D')
        switch (op) {
        case 0x58: e(FADD(d, a, c)); break;
        case 0x59: e(FMUL(d, a, c)); break;
        case 0x5c: e(FSUB(d, a, c)); break;
        default: e(FDIV(d, a, c)); break;
        }
    else
        switch (op) {
        case 0x58: e(FADDS(d, a, c)); break;
        case 0x59: e(FMULS(d, a, c)); break;
        case 0x5c: e(FSUBS(d, a, c)); break;
        default: e(FDIVS(d, a, c)); break;
        }
}

/* movapd/movaps between registers: low element in the FPR, the rest in memory (or "becomes 0" inherited) */
static void xmm_copy(int from, int after);
static void xf_drop(int n);
static void xf_reg_copy(int s, int d)
{
    if (!g_xf[s].kind) {                     /* low half not in the cache: copy 16 bytes */
        int h = g_xf[s].h, hs = g_xf[s].hs, hhome = g_xf[s].hhome;
        xf_drop(d);
        xmm_copy(s, d);
        if (h) {                            /* high half of s in the FPR (or pending): d likewise, dirty right away */
            if (hhome) g_xf[d].hhome = (short)hhome; else e(FMR(XFH(d), XFH(s)));
            g_xf[d].h = 1; g_xf[d].hs = (char)hs;
        }
        return;
    }
    if (g_xf[s].pending) g_xf[d].pending = g_xf[s].pending;   /* both wait for the same slot */
    else { e(FMR(XF(d), XF(s))); g_xf[d].pending = 0; }
    if (g_xf[s].high) { g_xf[d].high = 1; xfh_drop(d); }   /* high half of s is a (not yet written) 0: d likewise */
    else if (g_xf[s].h) {                   /* high half in the FPR (for float: bytes 4..7 in memory) */
        if (g_xf[s].kind == 'F') { e(LWZ(3, xmm_off(s) + 4, RCPU)); e(STW(3, xmm_off(d) + 4, RCPU)); }
        if (g_xf[s].hhome) g_xf[d].hhome = g_xf[s].hhome; else { e(FMR(XFH(d), XFH(s))); g_xf[d].hhome = 0; }
        g_xf[d].h = 1; g_xf[d].hs = 1; g_xf[d].high = 0;
    } else {                                /* copy the high half in memory (from byte 8 or 4) */
        int o = g_xf[s].kind == 'D' ? 8 : 4;
        for (; o < 16; o += 4) { e(LWZ(3, xmm_off(s) + o, RCPU)); e(STW(3, xmm_off(d) + o, RCPU)); }
        g_xf[d].high = 0;
        xfh_drop(d);
    }
    g_xf[d].kind = g_xf[s].kind;
    g_xf[d].dirty = 1;
}

/* copy 16 bytes in Cpu.xmm (source in Intel format, destination valid there afterwards) */
static void xmm_copy(int from, int after)
{
    xmm_mat_code(from);
    e(LI(0, xmm_off(from))); e(LVX(0, RCPU, 0));
    e(LI(0, xmm_off(after))); e(STVX(0, RCPU, 0));
    xmm_new_intel(after);
}

/* xorpd/xorps with sign mask (x = 1) or andpd/andps with abs mask in the low element: fneg/fabs on the FPR;
   the remaining bytes of xmm d get the same operation with the mask in memory (Cpu.xmm, Intel byte order) */
static void masks_op(const Insn *b, int d, int x)
{
    char la = b->p66 ? 'D' : 'F';
    int s = xf_fetch(d, la), w;
    e(x ? FNEG(s, s) : FABS(s, s));
    g_xf[d].dirty = 1;
    if (g_xf[d].h) {                        /* high half in the FPR: the same mask there if it has one of these forms */
        uint64_t m = ld64(g_kaddr + 8);
        if (la == 'D' && (x ? (m == 0 || m == 0x8000000000000000ull) :
                              (m == ~0ull || m == 0x7fffffffffffffffull || m == 0))) {
            if (x ? m != 0 : m != ~0ull) {
                int f = XFH(d);
                xfh_done(d);
                if (m == 0) e(FSUB(f, F_MAGIC, F_MAGIC)); else e(x ? FNEG(f, f) : FABS(f, f));
                g_xf[d].hs = 1;
            }
            g_st_masks++;
            return;
        }
        xfh_back(d);                     /* otherwise in memory */
        xfh_drop(d);
    }
    for (w = la == 'D' ? 2 : 1; w < 4; w++) {
        uint32_t m = swap32(ld32(g_kaddr + 4 * (uint32_t)w));   /* the way lwz reads the word from Cpu.xmm */
        int off = xmm_off(d) + 4 * w;
        if (g_xf[d].high) {                 /* high half is the not yet written 0 */
            if (!x) continue;               /* 0 and m = 0: stays pending */
            load_imm(3, m);
            e(STW(3, off, RCPU));
            continue;
        }
        if (x ? m == 0 : m == 0xffffffffu) continue;
        if (!x && m == 0) { e(LI(0, 0)); e(STW(0, off, RCPU)); continue; }
        e(LWZ(3, off, RCPU));
        if (x) { if (m & 0xffff) e(XORI(3, 3, m & 0xffff)); if (m >> 16) e(XORIS(3, 3, m >> 16)); }
        else { load_imm(4, m); e(AND(3, 3, 4)); }
        e(STW(3, off, RCPU));
    }
    if (x) g_xf[d].high = 0;
    g_st_masks++;
}

/* ---------- Packed doubles (SSE2) ----------
 * Low double as with the scalar instructions (f14+n, g_xf), high double in f24+n (h, hs, hhome) or, without cache,
 * through Cpu.xmm. AltiVec swaps memory operands (16 bytes) once into a storage slot: both halves as doubles in
 * host byte order, one lfd per half. */
static int pd_can(const Insn *b)
{
    int op = b->op, o = op & 0xff, p = b->p66 && !b->rep;
    if (op < OP_0F || op > (OP_0F | 0xff) || g_no_pd) return 0;
    switch (o) {
    case 0x58: case 0x59: case 0x5c: case 0x5e: case 0x51:
        return p;
    case 0x5d: case 0x5f:                   /* minpd/maxpd, minsd/maxsd */
        return p || (b->rep == 0xf2 && !b->p66);
    case 0x10: case 0x11:                   /* movupd */
        return p;
    case 0x12: case 0x13: case 0x16: case 0x17:     /* movlpd/movhpd, movlps/movhps, movhlps/movlhps */
        return !b->rep && (b->mem || (!b->p66 && (o == 0x12 || o == 0x16)));
    case 0x14: case 0x15: case 0xc6:        /* unpcklpd, unpckhpd, shufpd */
        return p;
    case 0xc2:                              /* cmppd, cmpsd */
        return p || (b->rep == 0xf2 && !b->p66);
    case 0x5a:                              /* cvtps2pd, cvtpd2ps (with f2/f3: scalar, gen_fp) */
        return !b->rep;
    case 0x50:                              /* movmskpd */
        return p && !b->mem;
    case 0xe6:                              /* cvtdq2pd (f3), cvttpd2dq (66), cvtpd2dq (f2) */
        return p || ((b->rep == 0xf3 || b->rep == 0xf2) && !b->p66);
    case 0x7e:                              /* movq xmm, xmm/m64 */
        return b->rep == 0xf3 && !b->p66;
    case 0xd6:                              /* movq xmm/m64, xmm */
        return p;
    }
    return 0;
}

/* memory operand (16 bytes) goes through pd_slot (hoisting and constants possible) */
static int pd_slot_op(const Insn *b)
{
    int o = b->op & 0xff, p = b->p66 && !b->rep;
    if (!b->mem || g_no_pd || b->op < OP_0F || b->op > (OP_0F | 0xff)) return 0;
    switch (o) {
    case 0x58: case 0x59: case 0x5c: case 0x5e: case 0x5d: case 0x5f: case 0x51: case 0x14: case 0x15: case 0xc6: case 0xc2:
    case 0x5a:
        return p;
    case 0xe6:
        return p || (b->rep == 0xf2 && !b->p66);
    case 0x28: case 0x10:                   /* movapd, movupd (only with high halves in the FPR) */
        return p && g_xfh_on;
    }
    return 0;
}

/* 16 bytes at the memory operand of b into storage slot z, each double swapped separately */
static void pd_swap(const Insn *b, int z)
{
    int ra, rb, a;
    if (b->op != (OP_0F | 0x10)) {          /* aligned (movupd is not) */
        ea_pair(b, 1, &ra, &rb);
        e(LVX(0, ra, rb));
        e(LI(11, (int)OFF(vswapp))); e(LVX(2, RCPU, 11));
        e(VPERM(3, 0, 0, 2));
    } else {
        a = ea_in(b);
        e(LI(0, 15));
        e(LVX(1, a, 0));
        e(LVX(0, 0, a));
        e(RLWINM(11, a, 4, 24, 27));
        e(ADDI(11, 11, (int)OFF(vswapp)));
        e(LVX(2, RCPU, 11));
        e(VPERM(3, 0, 1, 2));
    }
    e(LI(0, z)); e(STVX(3, RCPU, 0));
}

static int pd_slot(const Insn *b, int aligned)
{
    int z;
    (void)aligned;
    if (g_pre_z && g_pre_kind == 'P') { z = g_pre_z; g_pre_z = 0; return z; }
    z = vz_new();
    pd_swap(b, z);
    return z;
}

/* constant (g_kaddr, 16 bytes) into FPR lo and hi; 0: pool full */
static int pd_const(int lo, int hi)
{
    return pool_load_at(lo, g_kaddr, 'D') && pool_load_at(hi, g_kaddr + 8, 'D');
}

/* second operand: register, constant or memory swapped once */
typedef struct { int reg, z, k; } PdQ;

static PdQ pd_q(const Insn *b)
{
    PdQ q;
    q.reg = b->mem ? -1 : b->rm; q.z = 0; q.k = b->mem && g_kaddr;
    if (b->mem && !q.k) q.z = pd_slot(b, 1);
    return q;
}

/* half of the second operand (high: 1) as an FPR; t: helper register */
static int pd_q_fetch(const Insn *b, PdQ *q, int top, int t)
{
    if (q->reg >= 0) return top ? xfh_fetch(q->reg, t) : xf_fetch(q->reg, 'D');
    if (q->k) {
        if (pool_load_at(t, g_kaddr + 8 * (uint32_t)top, 'D')) return t;
        q->k = 0;
        q->z = pd_slot(b, 1);
    }
    e(LFD(t, q->z + 8 * top, RCPU));
    return t;
}

/* FPR f (f2..f13) as a helper register: a forwarding entry there loses it */
static void fw_fpr_take(int f)
{
    int k = f - 2;
    if (k < 0 || k >= FW_N || !g_fw[k].valid || !g_fw[k].fpr_ok) return;
    g_fw[k].fpr_ok = 0;
    if (!g_fw[k].shadow || g_fw[k].shadow == (int16_t)(OFF(fw_shadow) + 8 * k)) g_fw[k].valid = 0;
}

/* a = min/max(a, b) like Intel: min a < b ? a : b, max a > b ? a : b (NaN and equal zeros: b) */
static void pd_minmax(int o, int a, int b2)
{
    e(FCMPU(1, a, b2));
    e(BC_(BO_TRUE, 4 + (o == 0x5d ? CR_LT : CR_GT), 8));
    e(FMR(a, b2));
}

/* cvt(t)sd2si overflow: the G4 saturates to 0x7fffffff, Intel returns 0x80000000 (r: result, f: source). When
   rounding (fctiw) the rounded value counts: (f + 2^52 + 2^31) - (2^52 + 2^31) in FPR t (2147483647.6 overflows) */
static void cvt_overflow(int r, int f, int rnd, int t)
{
    uint32_t *s1, *s2;
    e(ADDIS(0, r, 0x8000));
    e(CMPWI(1, 0, -1));
    s1 = p; e(BC_(BO_FALSE, 4 + CR_EQ, 0));
    if (rnd) { e(FADD(t, f, F_MAGIC)); e(FSUB(t, t, F_MAGIC)); f = t; }
    e(FCMPU(1, f, F_2POW31));
    s2 = p; e(BC_(BO_TRUE, 4 + CR_LT, 0));
    e(LIS(r, 0x8000));
    bc_set(s1, p);
    bc_set(s2, p);
}

/* movq: d.low = s.low, high half 0 */
static void pd_movq_reg(int s, int d)
{
    int f = xf_fetch(s, 'D');
    if (d != s) { xf_drop(d); e(FMR(XF(d), f)); xf_set(d, 'D'); }
    xfh_drop(d);
    g_xf[d].high = 1;
    g_xf[d].dirty = 1;                  /* high is written only together with the low half */
}

static void gen_fp(const Insn *b);

/* xmm n as two swapped doubles (host byte order, as in the storage slot) into vector register vr (v0 or v1) */
static void pd_vector(int n, int vr)
{
    if (g_xf[n].kind == 'D' && g_xf[n].pending && g_xf[n].h && g_xf[n].hhome == g_xf[n].pending + 8) {
        e(LI(0, g_xf[n].pending)); e(LVX(vr, RCPU, 0));     /* still together in the slot */
        return;
    }
    if (g_xf[n].kind == 'D' && g_xf[n].h && !g_xf[n].high) {
        int z;
        xf_done(n); xfh_done(n);
        z = vz_new();
        e(STFD(XF(n), z, RCPU)); e(STFD(XFH(n), z + 8, RCPU));
        e(LI(0, z)); e(LVX(vr, RCPU, 0));
        return;
    }
    xf_back(n);                          /* Cpu.xmm in Intel format: swap */
    e(LI(0, xmm_off(n))); e(LVX(vr, RCPU, 0));
    e(LI(11, (int)OFF(vswapp))); e(LVX(2, RCPU, 11));
    e(VPERM(vr, vr, vr, 2));
}

/* andpd/andnpd/orpd/xorpd on the swapped form (bitwise: the swap does not matter); the result waits in a
   storage slot for lfd (instead of going through the Intel format in Cpu.xmm and back) */
static void pd_bitwise(const Insn *b, int op)
{
    int d = b->reg, z, ra, rb;
    pd_vector(d, 0);
    if (b->mem) {
        ea_pair(b, 1, &ra, &rb);
        e(LVX(1, ra, rb));
        e(LI(11, (int)OFF(vswapp))); e(LVX(2, RCPU, 11));
        e(VPERM(1, 1, 1, 2));
    } else pd_vector(b->rm, 1);
    switch (op) {
    case 0x54: e(VAND(0, 0, 1)); break;
    case 0x55: e(VANDC(0, 1, 0)); break;
    case 0x56: e(PRE(0, 0, 1)); break;
    default: e(VXOR(0, 0, 1)); break;
    }
    z = vz_new();
    e(LI(0, z)); e(STVX(0, RCPU, 0));
    xf_drop(d);
    g_xf[d].kind = 'D'; g_xf[d].dirty = 1; g_xf[d].pending = (short)z;
    g_xf[d].h = 1; g_xf[d].hs = 1; g_xf[d].hhome = (short)(z + 8);
    g_st_pd++;
}

/* integers in r3 (low) and r4 (high) as doubles into XF(d) and the high half of d */
static void pd_from_int(int d)
{
    int za = tmp_new(), zb = tmp_new(), h = g_xfh_on ? XFH(d) : 1;
    e(XORIS(0, 3, 0x8000)); e(STW(0, za + 4, 1));
    e(XORIS(0, 4, 0x8000)); e(STW(0, zb + 4, 1));
    e(LIS(0, 0x4330)); e(STW(0, za, 1)); e(STW(0, zb, 1));
    e(LFD(0, za, 1)); e(LFD(1, zb, 1));
    xf_set_pre(d, 'D');
    e(FSUB(XF(d), 0, F_MAGIC));
    xf_set(d, 'D');
    e(FSUB(h, 1, F_MAGIC));
    xfh_set(d, h);
}

static void gen_pd(const Insn *b)
{
    int o = b->op & 0xff, d = b->reg, f, g, a, r;
    PdQ q;
    g_st_pd++;
    switch (o) {
    case 0x58: case 0x59: case 0x5c: case 0x5e: case 0x5d: case 0x5f:
        if (b->rep == 0xf2) {               /* minsd/maxsd */
            int fs;
            if (b->mem) { m_load(0, b, 'D'); fs = 0; } else fs = xf_fetch(b->rm, 'D');
            f = xf_fetch(d, 'D');
            pd_minmax(o, f, fs);
            xf_set(d, 'D');
            return;
        }
        q = pd_q(b);
        g = pd_q_fetch(b, &q, 1, 1);        /* high */
        f = xfh_fetch(d, 0);
        if (o == 0x5d || o == 0x5f) pd_minmax(o, f, g); else fp_calc(o, 'D', f, f, g);
        xfh_set(d, f);
        g = pd_q_fetch(b, &q, 0, 0);        /* low */
        f = xf_fetch(d, 'D');
        if (o == 0x5d || o == 0x5f) pd_minmax(o, f, g); else fp_calc(o, 'D', f, f, g);
        xf_set(d, 'D');
        return;
    case 0x51:                              /* sqrtpd: twice through the library */
        q = pd_q(b);
        g_c_pure = 1;
        g = pd_q_fetch(b, &q, 0, 1);
        if (g != 1) e(FMR(1, g));
        c_call((void *)sqrt);
        xf_set_pre(d, 'D');
        e(FMR(XF(d), 1));
        xf_set(d, 'D');
        g = pd_q_fetch(b, &q, 1, 1);
        if (g != 1) e(FMR(1, g));
        c_call((void *)sqrt);
        xfh_set(d, 1);
        g_c_pure = 0;
        return;
    case 0x10:                              /* movupd xmm, xmm/m128 */
        if (!b->mem) { if (b->rm != d) xf_reg_copy(b->rm, d); return; }
        xf_drop(d);
        if (g_xfh_on) {
            int z;
            if (g_kaddr && pd_const(XF(d), XFH(d))) {
                g_xf[d].kind = 'D'; g_xf[d].dirty = 1; g_xf[d].h = 1; g_xf[d].hs = 1;
                return;
            }
            z = pd_slot(b, 0);
            g_xf[d].kind = 'D'; g_xf[d].dirty = 1; g_xf[d].pending = (short)z;
            g_xf[d].h = 1; g_xf[d].hs = 1; g_xf[d].hhome = (short)(z + 8);
            return;
        }
        a = ea_in(b);                      /* Intel bytes unchanged into Cpu.xmm */
        for (r = 0; r < 16; r += 4) { e(LWZ(3, r, a)); e(STW(3, xmm_off(d) + r, RCPU)); }
        xmm_new_intel(d);
        return;
    case 0x11:                              /* movupd xmm/m128, xmm */
        if (!b->mem) { if (b->rm != d) xf_reg_copy(d, b->rm); return; }
        if (g_xf[d].kind == 'D' && g_xf[d].h && !g_xf[d].high) {
            xf_done(d); xfh_done(d);
            e(STFD(XF(d), TMP(0), 1)); e(STFD(XFH(d), TMP(1), 1));
            e(LWZ(3, TMP(0), 1)); e(LWZ(4, TMP(0) + 4, 1)); e(LWZ(5, TMP(1), 1)); e(LWZ(6, TMP(1) + 4, 1));
            a = ea_in(b);
            e(STWBRX(4, 0, a)); e(LI(0, 4)); e(STWBRX(3, a, 0));
            e(LI(0, 8)); e(STWBRX(6, a, 0)); e(LI(0, 12)); e(STWBRX(5, a, 0));
        } else {
            xf_back(d);                  /* Cpu.xmm in Intel format (also from the host form) */
            a = ea_in(b);
            for (r = 0; r < 16; r += 4) { e(LWZ(3, xmm_off(d) + r, RCPU)); e(STW(3, r, a)); }
        }
        after_save(b, 16, 0, a, -1);
        return;
    case 0x12:                              /* movlpd/movlps xmm, m64; movhlps xmm, xmm */
        if (!b->mem) {
            f = xfh_fetch(b->rm, 1);
            xf_set_pre(d, 'D');
            e(FMR(XF(d), f));
            xf_set(d, 'D');
            return;
        }
        {                                   /* bytes 0..7 new (for float also 4..7); high then applies to 8..15 */
            int qf, z = 0;
            if (!(g_kaddr && pool_load(XF(d), 'D'))) {
                if ((qf = fw_find(b, 8)) >= 0) e(FMR(XF(d), qf));
                else z = m_pre(b, 'D');
            }
            g_xf[d].kind = 'D'; g_xf[d].dirty = 1; g_xf[d].pending = (short)z;
        }
        return;
    case 0x13: {                            /* movlpd/movlps m64, xmm: like movsd */
        Insn t = *b;
        t.op = OP_0F | 0x11; t.rep = 0xf2; t.p66 = 0; t.wm_ok = 0;
        g_st_pd--;
        gen_fp(&t);
        return;
    }
    case 0x16:                              /* movhpd/movhps xmm, m64; movlhps xmm, xmm */
        if (!b->mem) { f = xf_fetch(b->rm, 'D'); xfh_set(d, f); return; }
        if (g_xfh_on) {
            int qf;
            if (g_kaddr && pool_load(XFH(d), 'D')) { xfh_set(d, XFH(d)); return; }
            if ((qf = fw_find(b, 8)) >= 0) { xfh_set(d, qf); return; }
            xfh_set_slot(d, m_pre(b, 'D'));
            return;
        }
        high_drop(d);
        a = ea_in(b);                      /* Intel bytes unchanged */
        e(LWZ(3, 0, a)); e(LWZ(4, 4, a));
        e(STW(3, xmm_off(d) + 8, RCPU)); e(STW(4, xmm_off(d) + 12, RCPU));
        return;
    case 0x17:                              /* movhpd/movhps m64, xmm */
        if (g_xf[d].h && !g_xf[d].hhome) { m_save(XFH(d), b, 'D'); return; }
        if (g_xf[d].h) {
            int z = g_xf[d].hhome;
            e(LWZ(3, z, RCPU)); e(LWZ(4, z + 4, RCPU));
            a = ea_in(b);
            tmp_save(z, 'D', a);
        } else if (g_xf[d].high) {
            a = ea_in(b);
            e(LI(0, 0)); e(STW(0, 0, a)); e(STW(0, 4, a));
        } else {
            a = ea_in(b);
            e(LWZ(3, xmm_off(d) + 8, RCPU)); e(LWZ(4, xmm_off(d) + 12, RCPU));
            e(STW(3, 0, a)); e(STW(4, 4, a));
        }
        after_save(b, 8, 0, a, -1);
        return;
    case 0x14:                              /* unpcklpd: high = s.low */
        q = pd_q(b);
        g = pd_q_fetch(b, &q, 0, 1);
        xfh_set(d, g);
        return;
    case 0x15:                              /* unpckhpd: low = d.high, high = s.high */
        q = pd_q(b);
        g = pd_q_fetch(b, &q, 1, 1);
        f = xfh_fetch(d, 0);
        xf_set_pre(d, 'D');
        e(FMR(XF(d), f));
        xf_set(d, 'D');
        xfh_set(d, g);
        return;
    case 0xc6: {                            /* shufpd: low = imm&1 ? d.high : d.low, high = imm&2 ? s.high : s.low */
        int i = (int)(b->imm & 3);
        q = pd_q(b);
        f = (i & 1) ? xfh_fetch(d, 0) : xf_fetch(d, 'D');
        g = pd_q_fetch(b, &q, i >> 1, 1);
        if (g == XF(d) && f != XF(d)) { e(FMR(1, g)); g = 1; }
        if (f != XF(d)) { xf_set_pre(d, 'D'); e(FMR(XF(d), f)); xf_set(d, 'D'); }
        xfh_set(d, g);
        return;
    }
    case 0xc2: {                            /* cmppd: mask from fcmpu per half, both into Cpu.xmm */
        int pr = (int)(b->imm & 7), bit = 4 + ((pr & 3) == 0 ? CR_EQ : (pr & 3) == 3 ? CR_UN : CR_LT), page;
        if (b->rep == 0xf2) {               /* cmpsd: low only, high half stays */
            if (b->mem) { m_load(1, b, 'D'); g = 1; } else g = xf_fetch(b->rm, 'D');
            f = xf_fetch(d, 'D');
            e(FCMPU(1, f, g));
            if ((pr & 3) == 2) e(CROR(4 + CR_LT, 4 + CR_LT, 4 + CR_EQ));
            e(MFCR(3));
            e(RLWINM(3, 3, bit + 1, 31, 31));
            e((pr & 4) ? ADDI(3, 3, -1) : NEG(3, 3));
            if (g_xf[d].high) { xf_zero_upper(d, 'D'); g_xf[d].high = 0; }
            g_xf[d].kind = 0; g_xf[d].pending = 0; g_xf[d].dirty = 0;
            e(STW(3, xmm_off(d), RCPU)); e(STW(3, xmm_off(d) + 4, RCPU));
            xmm_new_intel(d);
            return;
        }
        q = pd_q(b);
        for (page = 0; page < 2; page++) {
            r = 3 + page;
            f = page ? xfh_fetch(d, 0) : xf_fetch(d, 'D');
            g = pd_q_fetch(b, &q, page, 1);
            e(FCMPU(1, f, g));
            if ((pr & 3) == 2) e(CROR(4 + CR_LT, 4 + CR_LT, 4 + CR_EQ));
            e(MFCR(r));
            e(RLWINM(r, r, bit + 1, 31, 31));
            e((pr & 4) ? ADDI(r, r, -1) : NEG(r, r));
        }
        xf_drop(d);
        e(STW(3, xmm_off(d), RCPU)); e(STW(3, xmm_off(d) + 4, RCPU));
        e(STW(4, xmm_off(d) + 8, RCPU)); e(STW(4, xmm_off(d) + 12, RCPU));
        xmm_new_intel(d);
        return;
    }
    case 0x50: {                            /* movmskpd r32, xmm: signs of both halves */
        int n = b->rm;
        r = R(b->reg);
        if (g_xf[n].kind == 'D' && g_xf[n].pending) e(LWZ(3, g_xf[n].pending, RCPU));
        else if (g_xf[n].kind == 'D') { e(STFD(XF(n), TMP(0), 1)); e(LWZ(3, TMP(0), 1)); }
        else { xf_back_bottom(n); e(LBZ(3, xmm_off(n) + 7, RCPU)); e(RLWINM(3, 3, 24, 0, 31)); }
        if (g_xf[n].h && g_xf[n].hhome) e(LWZ(4, g_xf[n].hhome, RCPU));
        else if (g_xf[n].h) { e(STFD(XFH(n), TMP(1), 1)); e(LWZ(4, TMP(1), 1)); }
        else if (g_xf[n].high) e(LI(4, 0));
        else { e(LBZ(4, xmm_off(n) + 15, RCPU)); e(RLWINM(4, 4, 24, 0, 31)); }
        e(RLWINM(r, 3, 1, 31, 31));
        e(RLWIMI(r, 4, 2, 30, 30));
        return;
    }
    case 0xe6:
        if (b->rep == 0xf3) {               /* cvtdq2pd xmm, xmm/m64 */
            int n = b->rm;
            if (b->mem) { a = ea_in(b); e(LWBRX(3, 0, a)); e(LI(0, 4)); e(LWBRX(4, a, 0)); }
            else if (g_xf[n].kind == 'D' && g_xf[n].pending) {
                e(LWZ(3, g_xf[n].pending + 4, RCPU)); e(LWZ(4, g_xf[n].pending, RCPU));
            } else if (g_xf[n].kind == 'D') {
                e(STFD(XF(n), TMP(0), 1)); e(LWZ(3, TMP(0) + 4, 1)); e(LWZ(4, TMP(0), 1));
            } else {
                xf_back_bottom(n);
                e(LI(0, xmm_off(n))); e(LWBRX(3, RCPU, 0));
                e(LI(0, xmm_off(n) + 4)); e(LWBRX(4, RCPU, 0));
            }
            pd_from_int(d);
            return;
        }
        {                                   /* cvttpd2dq (66), cvtpd2dq (f2): two integers low, rest 0 */
            int trunc = b->p66;
            q = pd_q(b);
            fw_fpr_take(12); fw_fpr_take(13);
            f = pd_q_fetch(b, &q, 0, 0);
            g = pd_q_fetch(b, &q, 1, 1);
            e(trunc ? FCTIWZ(12, f) : FCTIW(12, f));
            e(trunc ? FCTIWZ(13, g) : FCTIW(13, g));
            e(STFD(12, TMP(0), 1)); e(STFD(13, TMP(1), 1));
            e(LWZ(3, TMP(0) + 4, 1)); e(LWZ(4, TMP(1) + 4, 1));
            cvt_overflow(3, f, !trunc, 12);
            cvt_overflow(4, g, !trunc, 12);
            xf_drop(d);
            e(ADDI(12, RCPU, xmm_off(d)));
            e(STWBRX(3, 0, 12)); e(LI(0, 4)); e(STWBRX(4, 12, 0));
            e(LI(0, 0)); e(STW(0, xmm_off(d) + 8, RCPU)); e(STW(0, xmm_off(d) + 12, RCPU));
            xmm_new_intel(d);
        }
        return;
    case 0x5a:
        if (!b->p66) {                      /* cvtps2pd: two floats (low) as doubles */
            int z, n = b->rm;
            if (b->mem) z = m_pre(b, 'D');  /* 8 bytes swapped: float 1 at z, float 0 at z + 4 */
            else if (g_xf[n].kind == 'D' && g_xf[n].pending) z = g_xf[n].pending;
            else if (g_xf[n].kind == 'D') { xf_done(n); z = vz_new(); e(STFD(XF(n), z, RCPU)); }
            else {
                xf_back_bottom(n);
                z = vz_new();
                e(LI(0, xmm_off(n))); e(LVX(0, RCPU, 0));
                e(LI(11, (int)OFF(vswap8))); e(LVX(2, RCPU, 11));
                e(VPERM(3, 0, 0, 2));
                e(LI(0, z)); e(STVX(3, RCPU, 0));
            }
            e(LFS(0, z + 4, RCPU)); e(LFS(1, z, RCPU));
            xf_set_pre(d, 'D');
            e(FMR(XF(d), 0));
            xf_set(d, 'D');
            xfh_set(d, 1);
            return;
        }
        q = pd_q(b);                        /* cvtpd2ps: two floats low, rest 0 */
        f = pd_q_fetch(b, &q, 0, 0);
        g = pd_q_fetch(b, &q, 1, 1);
        e(FRSP(1, g));
        e(STFS(1, TMP(0), 1));
        e(FRSP(0, f));
        xf_drop(d);
        e(LWZ(3, TMP(0), 1));
        e(LI(0, xmm_off(d) + 4)); e(STWBRX(3, RCPU, 0));
        e(LI(0, 0)); e(STW(0, xmm_off(d) + 8, RCPU)); e(STW(0, xmm_off(d) + 12, RCPU));
        e(FMR(XF(d), 0));
        xf_set(d, 'F');
        return;
    case 0x7e:                              /* movq xmm, xmm/m64: like movsd, high half 0 */
        if (b->mem) {
            Insn t = *b;
            t.op = OP_0F | 0x10; t.rep = 0xf2; t.p66 = 0; t.wm_ok = 0;
            g_st_pd--;
            gen_fp(&t);
            return;
        }
        pd_movq_reg(b->rm, d);
        return;
    case 0xd6:                              /* movq xmm/m64, xmm */
        if (b->mem) {
            Insn t = *b;
            t.op = OP_0F | 0x11; t.rep = 0xf2; t.p66 = 0; t.wm_ok = 0;
            g_st_pd--;
            gen_fp(&t);
            return;
        }
        pd_movq_reg(d, b->rm);
        return;
    }
}

static void gen_fp(const Insn *b)
{
    int op = b->op & 0xff, d = b->reg, fs, ra, rb, a;
    char kind = b->rep == 0xf2 ? 'D' : 'F';
    g_st_fp++;
    switch (op) {
    case 0x10:                              /* movsd/movss xmm, xmm/m (from memory: the rest becomes 0) */
        if (b->mem) {
            int z;
            xf_drop(d);
            if (g_vl_cur) { vl_load(XF(d), b); xf_set(d, kind); g_xf[d].high = 1; return; }
            if (g_kaddr && pool_load(XF(d), kind)) { xf_set(d, kind); g_xf[d].high = 1; return; }
            if ((z = fw_find(b, kind == 'D' ? 8 : 4)) >= 0) {   /* stored in the block: from the FPR */
                e(FMR(XF(d), z));
                xf_set(d, kind);
                g_xf[d].high = 1;
                return;
            }
            z = m_pre(b, kind);
            xf_set(d, kind);
            g_xf[d].high = 1;               /* zero the high half only at write-back */
            g_xf[d].pending = (short)z;  /* lfd only at first use */
            return;
        }
        fs = xf_fetch(b->rm, kind);
        xf_set_pre(d, kind);
        if (fs != XF(d)) e(FMR(XF(d), fs));
        xf_set(d, kind);
        return;
    case 0x11:                              /* movsd/movss xmm/m, xmm */
        if (!b->mem) {
            fs = xf_fetch(d, kind);
            xf_set_pre(b->rm, kind);
            if (fs != XF(b->rm)) e(FMR(XF(b->rm), fs));
            xf_set(b->rm, kind);
            return;
        }
        fw_write(b, kind == 'D' ? 8 : 4);
        if (g_xf[d].kind == kind && g_xf[d].pending) {     /* only copied: without the FPU from the scratch area */
            int z = g_xf[d].pending;
            e(LWZ(3, z, RCPU));
            if (kind == 'D') e(LWZ(4, z + 4, RCPU));
            a = ea_in(b);
            tmp_save(z, kind, a);
            after_save(b, kind == 'D' ? 8 : 4, 0, a, 3);
            fw_note_slot(b, kind == 'D' ? 8 : 4, z);     /* the swapped value is still in the storage slot */
            return;
        }
        if (g_xf[d].kind == kind) {
            int q;
            xf_done(d);
            m_save(XF(d), b, kind);
            if ((q = fw_new(b, kind == 'D' ? 8 : 4)) >= 0) { e(FMR(q, XF(d))); fw_shadow_write(q, kind == 'D' ? 8 : 4); }
            return;
        }
        xf_back(d);                      /* value is in memory: copy byte-swapped */
        e(LI(0, xmm_off(d))); e(LWBRX(3, RCPU, 0));
        if (kind == 'D') { e(LI(0, xmm_off(d) + 4)); e(LWBRX(4, RCPU, 0)); }
        a = ea_in(b);
        e(STWBRX(3, 0, a));
        if (kind == 'D') { e(LI(0, 4)); e(STWBRX(4, a, 0)); }
        after_save(b, kind == 'D' ? 8 : 4, 0, a, 3);
        return;
    case 0x58: case 0x59: case 0x5c: case 0x5e: {
        int q = b->mem && !g_kaddr && !g_vl_cur ? fw_find(b, kind == 'D' ? 8 : 4) : -1;   /* if stored in the block */
        int k = b->mem && (g_kaddr || g_vl_cur || q >= 0), z = b->mem && !k ? m_pre(b, kind) : 0, fd = xf_fetch(d, kind);   /* memory first, lfd afterwards */
        if (q >= 0) fs = q;
        else if (b->mem && g_vl_cur) { vl_load(0, b); fs = 0; }
        else if (b->mem) {
            if (!(k && pool_load(0, kind))) {
                if (k) z = m_pre(b, kind);
                e(kind == 'D' ? LFD(0, z, RCPU) : LFS(0, z, RCPU));
            }
            fs = 0;
        } else fs = xf_fetch(b->rm, kind);
        fp_calc(op, kind, fd, fd, fs);
        xf_set(d, kind);
        return;
    }
    case 0x51:                              /* sqrt through the library (the G4 has no fsqrt) */
        if (b->mem) m_load(1, b, kind); else e(FMR(1, xf_fetch(b->rm, kind)));
        g_c_pure = 1;
        c_call((void *)sqrt);
        g_c_pure = 0;
        xf_set_pre(d, kind);
        e(kind == 'F' ? FRSP(XF(d), 1) : FMR(XF(d), 1));
        xf_set(d, kind);
        return;
    case 0x2a: {                            /* cvtsi2sd/ss: (2^52 + 2^31 + i) - (2^52 + 2^31), exact */
        int v, z;
        if (g_vl_cur) {                     /* counter of a loop: the bits are already in the ring (stored iterations earlier) */
            xf_set_pre(d, kind);
            vl_convert(XF(d));
            if (kind == 'F') e(FRSP(XF(d), XF(d)));
            xf_set(d, kind);
            return;
        }
        v = rm_val(b, 4, 4); z = tmp_new();
        e(XORIS(0, v, 0x8000));
        e(STW(0, z + 4, 1));
        e(LIS(0, 0x4330));
        e(STW(0, z, 1));
        e(LFD(0, z, 1));
        xf_set_pre(d, kind);
        e(FSUB(XF(d), 0, F_MAGIC));
        if (kind == 'F') e(FRSP(XF(d), XF(d)));
        xf_set(d, kind);
        return;
    }
    case 0x2c: case 0x2d: {                 /* cvt(t)sd2si/ss2si r32, xmm/m */
        int r = R(b->reg);
        if (b->mem) { m_load(0, b, kind); fs = 0; } else fs = xf_fetch(b->rm, kind);
        e(op == 0x2c ? FCTIWZ(1, fs) : FCTIW(1, fs));
        e(STFD(1, TMP(1), 1));             /* slot 1: fixed, never pending */
        e(LWZ(r, TMP(1) + 4, 1));
        cvt_overflow(r, fs, op == 0x2d, 1);    /* upward overflow: Intel returns 0x80000000 */
        return;
    }
    case 0x5a: {                            /* f2: cvtsd2ss, f3: cvtss2sd */
        char from = kind, after = kind == 'D' ? 'F' : 'D';
        if (b->mem) { m_load(0, b, from); fs = 0; } else fs = xf_fetch(b->rm, from);
        xf_set_pre(d, after);
        if (after == 'F') e(FRSP(XF(d), fs)); else if (fs != XF(d)) e(FMR(XF(d), fs));
        xf_set(d, after);
        return;
    }
    case 0x28:                              /* movaps/movapd xmm, xmm/m128 */
        if (b->mem) {
            xf_drop(d);
            if (b->p66 && g_xfh_on && !g_no_pd) {     /* doubles: both swapped into one slot, lfd at use */
                int z;
                if (g_kaddr && pd_const(XF(d), XFH(d))) {
                    g_xf[d].kind = 'D'; g_xf[d].dirty = 1; g_xf[d].h = 1; g_xf[d].hs = 1;
                    return;
                }
                z = pd_slot(b, 1);
                g_xf[d].kind = 'D'; g_xf[d].dirty = 1; g_xf[d].pending = (short)z;
                g_xf[d].h = 1; g_xf[d].hs = 1; g_xf[d].hhome = (short)(z + 8);
                return;
            }
            ea_pair(b, 1, &ra, &rb);
            e(LVX(0, ra, rb));
            e(LI(0, xmm_off(d))); e(STVX(0, RCPU, 0));
            xmm_new_intel(d);
            return;
        }
        if (b->rm == d) return;
        xf_reg_copy(b->rm, d);
        return;
    case 0x29:                              /* movaps/movapd xmm/m128, xmm */
        if (b->mem && g_xf[d].kind == 'D' && g_xf[d].h && !g_xf[d].high) {
            int z;                          /* both halves in the cache: into one slot, swapped on the way out */
            if (g_xf[d].pending && g_xf[d].hhome == g_xf[d].pending + 8) z = g_xf[d].pending;
            else {
                xf_done(d); xfh_done(d);
                z = vz_new();
                e(STFD(XF(d), z, RCPU)); e(STFD(XFH(d), z + 8, RCPU));
            }
            e(LI(0, z)); e(LVX(0, RCPU, 0));
            e(LI(11, (int)OFF(vswapp))); e(LVX(2, RCPU, 11));
            e(VPERM(0, 0, 0, 2));
            ea_pair(b, 1, &ra, &rb);
            e(STVX(0, ra, rb));
            after_save(b, 16, ra, rb, -1);
            g_st_pd++;
            return;
        }
        xf_back(d);
        if (b->mem) {
            e(LI(0, xmm_off(d))); e(LVX(0, RCPU, 0));
            ea_pair(b, 1, &ra, &rb);
            e(STVX(0, ra, rb));
            after_save(b, 16, ra, rb, -1);
            return;
        }
        if (b->rm != d) xf_reg_copy(d, b->rm);
        return;
    case 0x54: case 0x55: case 0x56: case 0x57:     /* and/andn/or/xor ps/pd: bitwise on AltiVec */
        if (op == 0x57 && !b->mem && b->rm == d) {  /* xorps/xorpd x, x: 0, high half as a pending 0 */
            xf_drop(d);
            e(FSUB(XF(d), F_MAGIC, F_MAGIC));
            xf_set(d, b->p66 ? 'D' : 'F');
            g_xf[d].high = 1;
            return;
        }
        if (b->mem && g_kaddr && (op == 0x54 || op == 0x57) && g_xf[d].kind == (b->p66 ? 'D' : 'F')) {
            masks_op(b, d, op == 0x57);    /* sign/abs: fneg/fabs in the FPR, high half in memory */
            return;
        }
        if (b->p66 && g_xfh_on && !g_no_pd) { pd_bitwise(b, op); return; }
        xf_back(d);
        if (!b->mem) xf_back(b->rm);
        e(LI(0, xmm_off(d))); e(LVX(0, RCPU, 0));
        if (b->mem) { ea_pair(b, 1, &ra, &rb); e(LVX(1, ra, rb)); }
        else { e(LI(0, xmm_off(b->rm))); e(LVX(1, RCPU, 0)); }
        switch (op) {
        case 0x54: e(VAND(0, 0, 1)); break;
        case 0x55: e(VANDC(0, 1, 0)); break;
        case 0x56: e(PRE(0, 0, 1)); break;
        default: e(VXOR(0, 0, 1)); break;
        }
        e(LI(0, xmm_off(d))); e(STVX(0, RCPU, 0));
        g_xf[d].kind = 0;
        g_xf[d].high = 0;
        xfh_drop(d);
        return;
    case 0x2e: case 0x2f:                   /* (u)comis without a direct reader: the interpreter sets the flags */
        xf_all_forget();
        gen_interpret(b, 0, TRAP);
        return;
    }
}

/* (u)comisd/(u)comiss with readers: fcmpu, one condition per reader. Intel: CF = LT|UN, ZF = EQ|UN, PF = UN,
   OF = SF = 0. Composite bits with cror in cr1. 0: not possible (nothing generated). */
static int fp_cmp_fusion(const Insn *b, const int *ks, int nk, Cond *conds)
{
    char kind = b->p66 ? 'D' : 'F';
    int fa, fb, j, freemask = 4, n_or = 0, x87 = is_x87_cmp(b);
    for (j = 0; j < nk; j++) { int kk = ks[j] >> 1; if (kk == 1 || kk == 2 || kk == 7) n_or++; }
    if (n_or > 4) return 0;
    g_st_fp++;
    if (x87) {                              /* fucomi/fcomi(p) st(0), st(i): same flags as ucomisd */
        if (g_x7.d < 0 || g_x7.d >= 8) x7_all_back();
        fa = x7_fetch(0);
        fb = x7_fetch(b->rm);
    } else {
        fa = xf_fetch(b->reg, kind);
        if (b->mem) { m_load(0, b, kind); fb = 0; } else fb = xf_fetch(b->rm, kind);
    }
    e(FCMPU(0, fa, fb));
    for (j = 0; j < nk; j++) {
        int kk = ks[j] >> 1, negated = ks[j] & 1;
        switch (kk) {
        case 0: case 4: case 6: conds[j] = cond_mk(-1, negated); break;     /* OF, SF, SF!=OF: always 0 */
        case 1: e(CROR(freemask, CR_LT, CR_UN)); conds[j] = cond_mk(freemask++, !negated); break;
        case 2: case 7: e(CROR(freemask, CR_EQ, CR_UN)); conds[j] = cond_mk(freemask++, !negated); break;
        case 3: conds[j] = cond_mk(CR_GT, negated); break;                  /* CF|ZF = not GT */
        default: conds[j] = cond_mk(CR_UN, !negated); break;                /* 5: PF */
        }
    }
    if (x87 && b->op == 0xdf) x7_pop();     /* fucomip/fcomip */
    return 1;
}

/* cmpsb with and without repz/repnz: loop directly on ecx/esi/edi (gcc -Os inlines strcmp with literals this way:
   dyld compares every segment and section name with "cld; repz cmpsb", until now every time through the
   interpreter). Step from DF in Cpu.fl, flags like "cmp byte [esi], [edi]" of the last compare go to memory; at
   ecx = 0 the old ones stay (they were stored beforehand). */
static void gen_cmpsb(const Insn *b)
{
    uint32_t *null = 0, *loop, *out;
    flags_put();
    if (g_sl_wb_now) sl_back_code(g_sl_wmask);   /* stack slots with deferred write-back: bring memory up to date */
    e(LWZ(3, OFF(fl), RCPU));
    e(RLWINM(5, 3, 23, 30, 30));            /* DF -> 2 */
    e(SUBFIC(5, 5, 1));                     /* step 1 or -1 */
    if (b->rep) { e(CMPWI(0, R(ECX), 0)); null = p; e(0); }
    loop = p;
    e(LBZ(3, 0, R(ESI)));
    e(LBZ(4, 0, R(EDI)));
    e(ADD(R(ESI), R(ESI), 5));
    e(ADD(R(EDI), R(EDI), 5));
    if (b->rep) {
        e(ADDI(R(ECX), R(ECX), -1));
        e(CMPLW(0, 3, 4));
        out = p;
        e(0);
        e(CMPWI(0, R(ECX), 0));
        e(BC_(BO_FALSE, CR_EQ, (uint32_t)(uintptr_t)loop - (uint32_t)(uintptr_t)p));
        *out = BC_(b->rep == 0xf3 ? BO_FALSE : BO_TRUE, CR_EQ, (uint32_t)(uintptr_t)p - (uint32_t)(uintptr_t)out);
    }
    e(LI(0, CC_SUB)); e(STW(0, OFF(cc_op), RCPU));
    e(LI(0, 1)); e(STW(0, OFF(cc_size), RCPU));
    e(STW(3, OFF(cc_a), RCPU));
    e(STW(4, OFF(cc_b), RCPU));
    e(SUBF(0, 4, 3));
    e(RLWINM(0, 0, 0, 24, 31));
    e(STW(0, OFF(cc_res), RCPU));
    if (null) *null = BC_(BO_TRUE, CR_EQ, (uint32_t)(uintptr_t)p - (uint32_t)(uintptr_t)null);
    fs.kind = -1;
    fs.stored = 1;
}

/* generate one (non-control) instruction */
static void gen(const Insn *b, int needs, int needs_cf)
{
    int op = b->op, sz = b->opsz, v;
    if (op >= OP_0F && sse_can(b)) { gen_sse(b); return; }
    if (op >= OP_0F && pd_can(b)) { gen_pd(b); return; }
    if (op >= OP_0F && fp_can(b)) { gen_fp(b); return; }
    if (x87_can(b)) {
        if (is_x87_cmp(b)) gen_fp_direct(b, 0, TRAP);  /* without a direct reader: the interpreter sets the flags */
        else gen_x87(b);
        return;
    }
    if (op < 0x40 && (op & 7) < 6) {
        int g = (op & 1) ? sz : 1, dir = (op & 7) < 2 ? 0 : (op & 7) < 4 ? 1 : 3;
        if (!needs && !g_no_narrow && alu_narrow_imm(b, op >> 3, g, dir)) return;
        gen_alu(b, op >> 3, g, dir, needs);
        return;
    }
    switch (op) {
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f:
        gen_inc_dec(b, op & 7, op >= 0x48, needs, needs_cf);
        return;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        if ((op & 7) == ESP) { e(MR(3, R(ESP))); push_reg(3); }
        else {
            push_reg(R(op & 7));
            if (g_sw_aesp_ok) sw_guest_save(g_sw_aesp - 4, op & 7);     /* for the later pop */
        }
        return;
    case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
        if ((op & 7) == ESP) e(LWBRX(R(ESP), 0, R(ESP)));
        else {
            int k = g_sw_aesp_ok ? sw_find(g_sw_aesp) : -1;
            if (k >= 0) sw_fetch(k, R(op & 7)); else e(LWBRX(R(op & 7), 0, R(ESP)));
            e(ADDI(R(ESP), R(ESP), 4));
        }
        return;
    case 0x68: case 0x6a:
        push_reg(imm_reg(b->imm, 3));
        if (g_sw_aesp_ok) sw_const(g_sw_aesp - 4, b->imm);
        return;
    case 0x69: case 0x6b:
        v = rm_val(b, 4, 4);
        gen_imul(b->reg, v, 0, 1, (int32_t)b->imm, needs);
        return;
    case OP_0F | 0xaf:
        v = rm_val(b, 4, 4);
        gen_imul(b->reg, R(b->reg), v, 0, 0, needs);
        return;
    case 0x80: case 0x81: case 0x83:
        if (!needs && !g_no_narrow && alu_narrow_imm(b, b->reg, op == 0x80 ? 1 : sz, 2)) return;
        gen_alu(b, b->reg, op == 0x80 ? 1 : sz, 2, needs);
        return;
    case 0x84: case 0x85:
        gen_alu(b, 8, op == 0x84 ? 1 : sz, 0, needs);
        return;
    case 0xa8: case 0xa9:
        gen_alu(b, 8, op == 0xa8 ? 1 : sz, 3, needs);
        return;
    case 0xf6:
        gen_alu(b, 8, 1, 2, needs);
        return;
    case 0xf7: {
        int ra = 0, rb = 0, only_reg = 0;
        if (b->reg == 0) { gen_alu(b, 8, 4, 2, needs); return; }
        if (b->reg >= 4) { gen_muldiv(b, needs); return; }
        if (b->mem) {
            int c = slot_reg(b, 4);
            only_reg = c >= 0 && g_sl_wb_now;
            if (!only_reg) rmw_addr(b, &ra, &rb);
            v = c >= 0 ? c : rmw_val(b, ra, rb);
        } else v = R(b->rm);
        if (b->reg == 2) e(NOR(v, v, v));
        else {
            if (needs) { e(LI(RA, 0)); e(MR(RB, v)); }
            e(NEG(v, v));
            if (needs) { e(MR(RE, v)); flags_new(CC_SUB, 4); }
        }
        if (b->mem && !only_reg) { save_p(v, ra, rb, 4); after_save(b, 4, ra, rb, v); }
        return;
    }
    case 0x87:
        if (b->mem) {                       /* xchg [m],r (locks): without a second thread a load and a store are enough */
            int c = slot_reg(b, 4);
            if (c >= 0 && g_sl_wb_now) { e(MR(3, c)); e(MR(c, R(b->reg))); e(MR(R(b->reg), 3)); return; }
            load_m(3, b, 4);
            save_m(R(b->reg), b, 4);
            e(MR(R(b->reg), 3));
            return;
        }
        e(MR(3, R(b->reg))); e(MR(R(b->reg), R(b->rm))); e(MR(R(b->rm), 3));
        return;
    case 0x88: case 0x89: {
        int g = op == 0x88 ? 1 : sz;
        int32_t off;
        if (b->mem) {
            save_m(reg_src(b->reg, g, 4), b, g);
            if (g == 4 && g_no_sw2 && sw_slot(b, &off)) {              /* stack slot now holds this register */
                if (b->base == ESP && g_sw_arg_copy) sw_guest_save(off, b->reg);   /* argument: copy survives */
                else sw_guest(off, b->reg);
            }
        } else reg_set(b->rm, g, reg_val(b->reg, g, 4));
        return;
    }
    case 0x8a: case 0x8b: {
        int g = op == 0x8a ? 1 : sz;
        if (b->mem) {
            if (g == 4) {
                int32_t off;
                int k;
                if (sw_slot(b, &off) && slot_reg(b, 4) < 0 && (k = sw_find(off)) >= 0) {
                    sw_fetch(k, R(b->reg));     /* pushed onto the stack in the block: without a load */
                    return;
                }
                load_m(R(b->reg), b, 4);
                if (sw_slot(b, &off)) sw_guest(off, b->reg);            /* reading again saves the load */
                return;
            }
            load_m(4, b, g);
            reg_set(b->reg, g, 4);
        } else reg_set(b->reg, g, reg_val(b->rm, g, 4));
        return;
    }
    case 0xa0: case 0xa1: case 0xa2: case 0xa3: {
        int g = (op & 1) ? sz : 1;
        if (op <= 0xa1) {
            if (g == 4) load_m(R(EAX), b, 4); else { load_m(4, b, g); reg_set(EAX, g, 4); }
        } else save_m(reg_src(EAX, g, 4), b, g);
        return;
    }
    case 0x8d:
        ea_after(b, R(b->reg));
        return;
    case 0x90:
        return;
    case 0xa6:
        gen_cmpsb(b);
        return;
    case 0xfc: case 0xfd:                   /* DF is always in Cpu.fl (not arithmetic) */
        e(LWZ(3, OFF(fl), RCPU));
        e(op == 0xfc ? RLWINM(3, 3, 0, 22, 20) : ORI(3, 3, F_DF));
        e(STW(3, OFF(fl), RCPU));
        return;
    case 0x98: e(EXTSH(R(EAX), R(EAX))); return;
    case 0x99: e(SRAWI(R(EDX), R(EAX), 31)); return;
    case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
        e(LI(4, b->imm & 0xff));
        reg_set(op & 7, 1, 4);
        return;
    case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
        if (sz == 2) { e(LI(4, b->imm & 0xffff)); reg_set(op & 7, 2, 4); return; }
        reg_imm(R(op & 7), b->imm);
        return;
    case 0xc6: case 0xc7: {
        int g = op == 0xc6 ? 1 : sz;
        if (!b->mem) {
            if (g == 4) reg_imm(R(b->rm), b->imm);
            else { e(LI(4, b->imm & (g == 1 ? 0xff : 0xffff))); reg_set(b->rm, g, 4); }
            return;
        }
        g_sw_no_note = 1;
        save_m(imm_reg(g == 1 ? (b->imm & 0xff) : g == 2 ? (b->imm & 0xffff) : b->imm, 4), b, g);
        g_sw_no_note = 0;
        {
            int32_t off;
            if (g == 4 && sw_slot(b, &off)) sw_const(off, b->imm);
        }
        return;
    }
    case 0xc1: case 0xd1: {
        int n = op == 0xd1 ? 1 : (int)(b->imm & 31);
        if (n == 0) return;
        if (b->reg <= 1) {                  /* rol/ror */
            if (g_afterwards_now & (FCF | FOF)) gen_interpret(b, 0, FCF | FOF);
            else if (sz == 2) gen_rotate16(b, b->reg == 1, n);
            else gen_rotate(b, b->reg == 1, n);
            return;
        }
        gen_shift(b, b->reg, n, needs);
        return;
    }
    case 0xd3:
        if (b->reg <= 1) {                  /* rol/ror by cl: count 0 leaves CF and OF, so the interpreter if they are live */
            if (g_afterwards_now & (FCF | FOF)) gen_interpret(b, 0, FCF | FOF);
            else gen_rotate_cl(b, b->reg == 1);
            return;
        }
        gen_shift(b, b->reg, -1, needs);
        return;
    case 0xc0: case 0xd0: {
        int n = op == 0xd0 ? 1 : (int)(b->imm & 31);
        if (n == 0) return;
        if (b->reg <= 1 && (g_afterwards_now & (FCF | FOF))) { gen_interpret(b, 0, FCF | FOF); return; }
        gen_shift8(b, b->reg, n, needs);
        return;
    }
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        e(MR(3, R(EAX))); e(MR(R(EAX), R(op & 7))); e(MR(R(op & 7), 3));
        return;
    case OP_0F | 0xa4: case OP_0F | 0xac: {
        int n = (int)(b->imm & 31);
        if (n == 0) return;
        /* OF of a one-bit shrd is not that of shr: the interpreter if it is needed */
        if (op == (OP_0F | 0xac) && n == 1 && (g_afterwards_now & FOF)) { gen_interpret(b, 0, TRAP); return; }
        gen_double_shift(b, op == (OP_0F | 0xa4), n, needs);
        return;
    }
    case OP_0F | 0xa5: case OP_0F | 0xad:
        /* live flags: the interpreter (count 0 leaves them, otherwise new ones); s != TRAP stores the current ones first */
        if (g_afterwards_now & TRAP) { gen_interpret(b, 0, TRAP & ~FAF); return; }
        gen_double_shift(b, op == (OP_0F | 0xa5), -1, 0);
        return;
    case 0xc9: {                            /* leave: esp = ebp, pop ebp */
        int k = g_sw_aebp_ok ? sw_find(g_sw_aebp) : -1;
        e(MR(R(ESP), R(EBP)));
        if (k >= 0) sw_fetch(k, R(EBP)); else e(LWBRX(R(EBP), 0, R(ESP)));
        e(ADDI(R(ESP), R(ESP), 4));
        return;
    }
    case 0xfe:                              /* incb, decb */
        gen_inc_dec(b, b->mem ? -1 : b->rm, b->reg, needs, needs_cf);
        return;
    case 0xff:
        if (b->reg <= 1) { gen_inc_dec(b, b->mem ? -1 : b->rm, b->reg, needs, needs_cf); return; }
        /* push r/m */
        v = rm_val(b, 4, 3);
        push_reg(v);
        return;
    case OP_0F | 0x18: case OP_0F | 0x19: case OP_0F | 0x1a: case OP_0F | 0x1b:
    case OP_0F | 0x1c: case OP_0F | 0x1d: case OP_0F | 0x1e: case OP_0F | 0x1f:
    case OP_0F | 0x77:                      /* emms */
        return;
    case OP_0F | 0x40: case OP_0F | 0x41: case OP_0F | 0x42: case OP_0F | 0x43:
    case OP_0F | 0x44: case OP_0F | 0x45: case OP_0F | 0x46: case OP_0F | 0x47:
    case OP_0F | 0x48: case OP_0F | 0x49: case OP_0F | 0x4a: case OP_0F | 0x4b:
    case OP_0F | 0x4c: case OP_0F | 0x4d: case OP_0F | 0x4e: case OP_0F | 0x4f: {
        /* condition first (a helper call may destroy r3..r12), then load: the load leaves CR0 unchanged */
        Cond c = cond_fetch(op & 15);
        v = rm_val(b, 4, 4);
        if (c.bit < 0) { if (c.truth) e(MR(R(b->reg), v)); return; }
        e(BC_(c.truth ? BO_FALSE : BO_TRUE, c.bit, 8));
        e(MR(R(b->reg), v));
        return;
    }
    case OP_0F | 0x90: case OP_0F | 0x91: case OP_0F | 0x92: case OP_0F | 0x93:
    case OP_0F | 0x94: case OP_0F | 0x95: case OP_0F | 0x96: case OP_0F | 0x97:
    case OP_0F | 0x98: case OP_0F | 0x99: case OP_0F | 0x9a: case OP_0F | 0x9b:
    case OP_0F | 0x9c: case OP_0F | 0x9d: case OP_0F | 0x9e: case OP_0F | 0x9f: {
        int unpredictable = g_ahead_i < g_ahead_n && g_ahead_data && !g_no_setcc_cr;
        Cond c = cond_fetch(op & 15);
        if (unpredictable) cond_val_cr(c, 4); else cond_val(c, 4);
    }
        if (b->mem) save_m(4, b, 1); else reg_set(b->rm, 1, 4);
        return;
    case OP_0F | 0xb6: case OP_0F | 0xb7: case OP_0F | 0xbe: case OP_0F | 0xbf: {
        int g = (op & 1) ? 2 : 1, sign = (op & 0xff) >= 0xbe, t = sz == 2 ? 4 : R(b->reg);   /* 16 bit: low half only */
        if (b->mem) load_m(t, b, g);
        else if (g == 2) e(RLWINM(t, R(b->rm), 0, 16, 31));
        else if (b->rm < 4) e(RLWINM(t, R(b->rm), 0, 24, 31));
        else e(RLWINM(t, R(b->rm - 4), 24, 24, 31));
        if (sign) e(g == 1 ? EXTSB(t, t) : EXTSH(t, t));
        if (sz == 2) reg_set(b->reg, 2, t);
        return;
    }
    case OP_0F | 0xbc: case OP_0F | 0xbd:   /* bsf, bsr: destination stays unchanged at 0; ZF from the source */
        v = rm_val(b, 4, 4);
        if (op == (OP_0F | 0xbc)) { e(NEG(0, v)); e(AND(0, 0, v)); e(CNTLZW(0, 0)); }
        else e(CNTLZW(0, v));
        e(SUBFIC(0, 0, 31));
        if (needs) e(MR(RE, v));
        e(CMPWI(1, v, 0));
        e(BC_(BO_TRUE, 4 + CR_EQ, 8));
        e(MR(R(b->reg), 0));
        if (needs) flags_new(CC_LOGIC, 4);
        return;
    case OP_0F | 0xc8: case OP_0F | 0xc9: case OP_0F | 0xca: case OP_0F | 0xcb:
    case OP_0F | 0xcc: case OP_0F | 0xcd: case OP_0F | 0xce: case OP_0F | 0xcf: {
        int r = R(op & 7);
        e(RLWINM(3, r, 8, 0, 31));
        e(RLWIMI(3, r, 24, 0, 7));
        e(RLWIMI(3, r, 24, 16, 23));
        e(MR(r, 3));
        return;
    }
    }
}

/* ---------- Choosing the stack slots ---------- */

/* memory access of a translatable instruction (without push/pop/call/ret): size, reads/writes. 0: none */
static int mem_access(const Insn *b, int *sz, int *reads, int *writes)
{
    int op = b->op;
    *sz = 4; *reads = 0; *writes = 0;
    if (x87_can(b)) {
        if (!b->mem) return 0;
        *sz = op == 0xdd || op == 0xdc ? 8 : op == 0xdf ? 2 : 4;
        if ((op == 0xd9 || op == 0xdd) && b->reg >= 2) *writes = 1;
        else if (op == 0xdb && b->reg >= 1) *writes = 1;
        else *reads = 1;
        return 1;
    }
    if (op >= OP_0F && pd_can(b)) {
        int o = op & 0xff;
        if (!b->mem) return 0;
        switch (o) {
        case 0x11: case 0x29: *sz = 16; *writes = 1; return 1;
        case 0x13: case 0x17: case 0xd6: *sz = 8; *writes = 1; return 1;
        case 0x12: case 0x16: case 0x7e: *sz = 8; *reads = 1; return 1;
        case 0x5d: case 0x5f: *sz = b->rep == 0xf2 ? 8 : 16; *reads = 1; return 1;
        case 0xe6: *sz = b->rep == 0xf3 ? 8 : 16; *reads = 1; return 1;
        case 0x5a: *sz = b->p66 ? 16 : 8; *reads = 1; return 1;
        case 0xc2: *sz = b->rep == 0xf2 ? 8 : 16; *reads = 1; return 1;
        }
        *sz = 16; *reads = 1;               /* arithmetic, movupd, unpck, shufpd, cmppd: read 16 bytes */
        return 1;
    }
    if (op >= OP_0F && sse_can(b)) {
        int o = op & 0xff;
        if (!b->mem) return 0;
        switch (o) {
        case 0x6e: *sz = 4; *reads = 1; return 1;                   /* movd xmm, m32 */
        case 0x7e: *sz = 4; *writes = 1; return 1;                  /* movd m32, xmm */
        case 0xc4: *sz = 2; *reads = 1; return 1;                   /* pinsrw */
        case 0x7f: case 0x11: case 0xe7: *sz = 16; *writes = 1; return 1;
        }
        *sz = 16; *reads = 1;
        return 1;
    }
    if (op >= OP_0F && (sse_can(b) || fp_can(b))) {
        int o = op & 0xff, d = b->rep == 0xf2 ? 8 : 4;
        if (!b->mem) return 0;
        switch (o) {
        case 0x10: *sz = d; *reads = 1; return 1;
        case 0x11: *sz = d; *writes = 1; return 1;
        case 0x2a: *reads = 1; return 1;
        case 0x6f: case 0x28: case 0x54: case 0x55: case 0x56: case 0x57: case 0x74: case 0x75: case 0x76:
        case 0xdb: case 0xdf: case 0xeb: case 0xef: *sz = 16; *reads = 1; return 1;
        case 0x7f: case 0x29: *sz = 16; *writes = 1; return 1;
        case 0x2e: case 0x2f: *sz = b->p66 ? 8 : 4; *reads = 1; return 1;
        }
        *sz = d; *reads = 1;                /* arithmetic, sqrt, cvt: read the memory operand */
        return 1;
    }
    switch (op) {
    case 0xa0: case 0xa1: *sz = op == 0xa0 ? 1 : b->opsz; *reads = 1; return 1;
    case 0xa2: case 0xa3: *sz = op == 0xa2 ? 1 : b->opsz; *writes = 1; return 1;
    }
    if (!b->mem || op == 0x8d || (op >= (OP_0F | 0x18) && op <= (OP_0F | 0x1f))) return 0;
    if (op < 0x40 && (op & 7) < 6) {
        *sz = (op & 1) ? b->opsz : 1;
        *reads = 1;
        *writes = (op & 7) < 2 && (op >> 3) != 7;
        return 1;
    }
    switch (op) {
    case 0x80: case 0x81: case 0x83: *sz = op == 0x80 ? 1 : b->opsz; *reads = 1; *writes = b->reg != 7; return 1;
    case 0x84: case 0x85: *sz = op == 0x84 ? 1 : b->opsz; *reads = 1; return 1;
    case 0xf6: *sz = 1; *reads = 1; return 1;
    case 0xf7: *reads = 1; *writes = b->reg == 2 || b->reg == 3; return 1;
    case 0x88: case 0x89: *sz = op == 0x88 ? 1 : b->opsz; *writes = 1; return 1;
    case 0x8a: case 0x8b: *sz = op == 0x8a ? 1 : b->opsz; *reads = 1; return 1;
    case 0xc6: case 0xc7: *sz = op == 0xc6 ? 1 : b->opsz; *writes = 1; return 1;
    case 0xc1: case 0xd1: case 0xd3: *sz = b->opsz; *reads = 1; *writes = 1; return 1;
    case 0xc0: case 0xd0: case 0xd2: *sz = 1; *reads = 1; *writes = 1; return 1;
    case 0xff: if (b->reg <= 1) *sz = b->opsz; *reads = 1; *writes = b->reg <= 1; return 1;
    case 0xfe: *sz = 1; *reads = 1; *writes = 1; return 1;
    case 0x87: *reads = 1; *writes = 1; return 1;
    case 0x69: case 0x6b: case OP_0F | 0xaf: *reads = 1; return 1;
    case OP_0F | 0xb6: case OP_0F | 0xbe: *sz = 1; *reads = 1; return 1;
    case OP_0F | 0xb7: case OP_0F | 0xbf: *sz = 2; *reads = 1; return 1;
    }
    if (op >= (OP_0F | 0x40) && op <= (OP_0F | 0x4f)) { *reads = 1; return 1; }
    if (op >= (OP_0F | 0x90) && op <= (OP_0F | 0x9f)) { *sz = 1; *writes = 1; return 1; }
    *reads = 1; *writes = 1;              /* otherwise: both, to be safe */
    return 1;
}

/* Intel registers the instruction can change (bit mask; unknown instructions: all). The result depends only on
   op, mem, reg, rm, opsz and is cached in the instruction (whoever changes these fields resets wm_ok). */
static uint8_t write_mask_calc(const Insn *b);
static uint8_t write_mask(const Insn *b)
{
    Insn *w = (Insn *)b;
    if (!b->wm_ok) { w->wm = write_mask_calc(b); w->wm_ok = 1; }
    return b->wm;
}

static uint8_t write_mask_calc(const Insn *b)
{
    int op = b->op, m = b->mem;
#define BREG(n, g) ((g) == 1 && (n) >= 4 ? (n) - 4 : (n))
#define MK(x) ((uint8_t)(1u << (x)))
    if (x87_can(b)) return 0;
    if (op >= OP_0F && pd_can(b)) return (op & 0xff) == 0x50 ? MK(b->reg) : 0;    /* movmskpd */
    if (op >= OP_0F && sse_can(b)) {      /* pmovmskb, pextrw, movd r32 write an integer register */
        if ((op & 0xff) == 0xd7 || (op & 0xff) == 0xc5) return MK(b->reg);
        return (op & 0xff) == 0x7e && !m ? MK(b->rm) : 0;
    }
    if (op >= OP_0F && fp_can(b))          /* only cvt*2si write an integer register */
        return ((op & 0xff) == 0x2c || (op & 0xff) == 0x2d) ? MK(b->reg) : 0;
    if (op < 0x40 && (op & 7) < 6) {
        int g = (op & 1) ? b->opsz : 1;
        if ((op >> 3) == 7) return 0;
        if ((op & 7) >= 4) return MK(EAX);
        if ((op & 7) >= 2) return MK(BREG(b->reg, g));
        return m ? 0 : MK(BREG(b->rm, g));
    }
    if (op >= 0x40 && op <= 0x4f) return MK(op & 7);
    if (op >= 0x50 && op <= 0x57) return MK(ESP);
    if (op >= 0x58 && op <= 0x5f) return MK(op & 7) | MK(ESP);
    if (op >= 0x70 && op <= 0x7f) return 0;
    if (op >= 0xb0 && op <= 0xb7) return MK(BREG(op & 7, 1));
    if (op >= 0xb8 && op <= 0xbf) return MK(op & 7);
    if (op >= (OP_0F | 0x80) && op <= (OP_0F | 0x8f)) return 0;
    if (op >= (OP_0F | 0x40) && op <= (OP_0F | 0x4f)) return MK(b->reg);
    if (op >= (OP_0F | 0x90) && op <= (OP_0F | 0x9f)) return m ? 0 : MK(BREG(b->rm, 1));
    if (op >= (OP_0F | 0xc8) && op <= (OP_0F | 0xcf)) return MK(op & 7);
    if (op >= (OP_0F | 0x18) && op <= (OP_0F | 0x1f)) return 0;
    switch (op) {
    case 0x68: case 0x6a: case 0xc2: case 0xc3: case 0xe8: return MK(ESP);
    case 0x69: case 0x6b: case OP_0F | 0xaf: case 0x8d: return MK(b->reg);
    case OP_0F | 0xb6: case OP_0F | 0xb7: case OP_0F | 0xbe: case OP_0F | 0xbf: return MK(b->reg);
    case 0x80: case 0x81: case 0x83: return b->reg != 7 && !m ? MK(BREG(b->rm, op == 0x80 ? 1 : b->opsz)) : 0;
    case 0x84: case 0x85: case 0xa8: case 0xa9: case 0x90: case 0xe9: case 0xeb: case 0xa2: case 0xa3: case 0xfc: case 0xfd: return 0;
    case 0xa0: case 0xa1: case 0x98: return MK(EAX);
    case 0x99: return MK(EDX);
    case 0x87: return m ? MK(b->reg) : MK(b->reg) | MK(b->rm);
    case 0x88: case 0x89: return m ? 0 : MK(BREG(b->rm, op == 0x88 ? 1 : b->opsz));
    case 0x8a: case 0x8b: return MK(BREG(b->reg, op == 0x8a ? 1 : b->opsz));
    case 0xc6: case 0xc7: return m ? 0 : MK(BREG(b->rm, op == 0xc6 ? 1 : b->opsz));
    case 0xc1: case 0xd1: case 0xd3: return m ? 0 : MK(b->rm);
    case OP_0F | 0xa4: case OP_0F | 0xa5: case OP_0F | 0xac: case OP_0F | 0xad: return m ? 0 : MK(b->rm);
    case 0xc0: case 0xd0: return m ? 0 : MK(BREG(b->rm, 1));
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: return MK(EAX) | MK(op & 7);
    case 0xc9: return MK(ESP) | MK(EBP);
    case 0xf6: case 0xf7:                   /* test; not/neg on r/m; mul/imul/div/idiv write eax (and edx) */
        if (b->reg == 0 || b->reg == 1) return 0;
        if (b->reg == 2 || b->reg == 3) return m ? 0 : MK(BREG(b->rm, op == 0xf6 ? 1 : 4));
        return op == 0xf6 ? MK(EAX) : (uint8_t)(MK(EAX) | MK(EDX));
    case 0xfe: return b->reg <= 1 && !m ? MK(BREG(b->rm, 1)) : (b->reg <= 1 ? 0 : 0xff);
    case 0xff:
        if (b->reg <= 1) return m ? 0 : MK(b->rm);
        if (b->reg == 2 || b->reg == 6) return MK(ESP);
        return 0;
    }
#undef MK
#undef BREG
    return 0xff;
}

/* 1 if the instruction can change the Intel register r */
static int writes_reg(const Insn *b, int r) { return write_mask(b) >> r & 1; }

/* writes to the stack without a memory operand (push, call) */
static int is_push(const Insn *b)
{
    int op = b->op;
    return (op >= 0x50 && op <= 0x57) || op == 0x68 || op == 0x6a || op == 0xe8 || (op == 0xff && (b->reg == 2 || b->reg == 6));
}

/* ---------- Deferred write-back: loop pointers tracked symbolically ----------
 * Every value in the loop part is "entry value of a source + constant" (source: register 0..7, slot 8..11,
 * SQ_K = pure constant) or unknown. An access whose address consists of sources that change by fixed steps per
 * iteration moves monotonically; if at entry it lies on the side it moves away from, it never hits the slots
 * (|step| < 4 KB: a wraparound across 0 would run through the unmapped page 0 and fault before that). */
#define SQ_K 12
typedef struct { int8_t q; int32_t c; } Sym;
/* Address = qb + (qx << s) + c; direction r: 0 fixed, -1 down, 1 up, 2 both; m = largest step.
   If an access runs toward the slots, the distance limits the number of iterations (budget in CTR: the
   backward jump instructions count along, bdnzt/bdnzf; when it is used up, execution re-enters through the block
   start). */
typedef struct { int8_t qb, qx, s, sz, r; int32_t c, m; } RCheck;
#define RP_MAX 12
static RCheck g_rp[RP_MAX];
static int g_rp_n;
static unsigned long g_st_ret, g_st_ret_miss, g_st_ret_check, g_st_budget;

static int is_ret(const Insn *x, int kind, int verf, uint32_t eip)
{
    return (kind == JUMP_COND || (kind == JUMP && !verf)) && x->eip + x->len + x->imm == eip;
}

/* index of the chosen slot that lies exactly at [basis + disp]; -1 */
static int slot_k(const Insn *x, int base)
{
    int k;
    if (!is_slot(x, base)) return -1;
    for (k = 0; k < g_sl_n; k++) if ((int32_t)x->disp == g_sl_d[k]) return k;
    return -1;
}

static int int_op(const Insn *x)
{
    return !x87_can(x) && !(x->op >= OP_0F && (sse_can(x) || fp_can(x) || pd_can(x)));
}

static Sym sym_mk(int q, int32_t c) { Sym v; v.q = (int8_t)q; v.c = c; return v; }

/* effect of an instruction on registers and slots (only what the pointer arithmetic needs; otherwise unknown) */
static void sym_step(const Insn *x, Sym *r, Sym *pl, int base)
{
    int op = x->op, k = x->mem ? slot_k(x, base) : -1, target = -1, zp = -1, j, sz, l, s;
    uint8_t m = write_mask(x);
    Sym v = sym_mk(-1, 0);
    if (x->opsz == 4 && !x->seg) switch (op) {
    case 0x89:
        if (x->mem) { if (k >= 0) { zp = k; v = r[x->reg]; } }
        else { target = x->rm; v = r[x->reg]; }
        break;
    case 0x8b:
        target = x->reg;
        if (!x->mem) v = r[x->rm]; else if (k >= 0) v = pl[k];
        break;
    case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
        target = op & 7; v = sym_mk(SQ_K, (int32_t)x->imm);
        break;
    case 0xc7:
        if (x->opsz != 4) break;            /* movw: destination unknown (mask or slot below) */
        v = sym_mk(SQ_K, (int32_t)x->imm);
        if (x->mem) zp = k; else target = x->rm;
        break;
    case 0x81: case 0x83:
        if (x->opsz == 4 && (x->reg == 0 || x->reg == 5)) {     /* 16 bit: unknown (mask below) */
            Sym q = x->mem ? (k >= 0 ? pl[k] : sym_mk(-1, 0)) : r[x->rm];
            if (q.q >= 0) v = sym_mk(q.q, (int32_t)((uint32_t)q.c + (x->reg == 0 ? x->imm : 0u - x->imm)));
            if (x->mem) zp = k; else target = x->rm;
        }
        break;
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f:
        target = op & 7;
        if (x->opsz == 4 && r[target].q >= 0) v = sym_mk(r[target].q, r[target].c + (op < 0x48 ? 1 : -1));
        break;
    case 0xff:
        if (x->reg <= 1) {
            Sym q = x->mem ? (k >= 0 ? pl[k] : sym_mk(-1, 0)) : r[x->rm];
            if (q.q >= 0) v = sym_mk(q.q, q.c + (x->reg == 0 ? 1 : -1));
            if (x->mem) zp = k; else target = x->rm;
        }
        break;
    case 0x8d: {
        Sym sb = x->base >= 0 ? r[x->base] : sym_mk(SQ_K, 0), sx = x->index >= 0 ? r[x->index] : sym_mk(SQ_K, 0);
        target = x->reg;
        if (sb.q < 0 || sx.q < 0) break;
        if (sx.q == SQ_K) v = sym_mk(sb.q, (int32_t)((uint32_t)sb.c + ((uint32_t)sx.c << x->scale) + x->disp));
        else if (sb.q == SQ_K && !x->scale) v = sym_mk(sx.q, (int32_t)((uint32_t)sx.c + (uint32_t)sb.c + x->disp));
        break;
    }
    case 0x01: case 0x03: case 0x29: case 0x2b: case 0x31: case 0x33:
        if (x->mem || x->opsz != 4) break;
        {
            int zr = (op & 2) ? x->reg : x->rm, qr = (op & 2) ? x->rm : x->reg;
            Sym a = r[zr], q = r[qr];
            target = zr;
            if (op >= 0x29 && zr == qr) v = sym_mk(SQ_K, 0);                     /* sub r,r / xor r,r */
            else if (op >= 0x31) break;
            else if (q.q == SQ_K && a.q >= 0) v = sym_mk(a.q, (int32_t)((uint32_t)a.c + (op < 0x29 ? (uint32_t)q.c : 0u - (uint32_t)q.c)));
            else if (op < 0x29 && a.q == SQ_K && q.q >= 0) v = sym_mk(q.q, (int32_t)((uint32_t)q.c + (uint32_t)a.c));
        }
        break;
    }
    for (j = 0; j < 8; j++) if (m >> j & 1) r[j] = sym_mk(-1, 0);
    if (k >= 0 && mem_access(x, &sz, &l, &s) && s) pl[k] = sym_mk(-1, 0);
    if (target >= 0) r[target] = v;
    if (zp >= 0) pl[zp] = v;
}

/* step of source q per iteration at the backward jump (state z); 0: q is not an induction variable */
static int sym_stride(const Sym *z, int q, int32_t *d)
{
    if (q == SQ_K) { *d = 0; return 1; }
    if (z[q].q != q) return 0;
    *d = z[q].c;
    return *d > -4096 && *d < 4096;
}

/* Whether the chosen slots (g_sl_*) can be kept only in the register in loop part 0..last. Sets g_rp, g_sl_wmask. */
static int ret_plan(const Insn *b, const int *kinds, const int *traced, const int *interp, uint32_t eip,
                        int base, int last)
{
    Sym r[8], pl[SLOTS], edge[8][8 + SLOTS];
    int i, j, k, nk = 0, wm = 0, inexact = 0, foreign_s = 0;
    if (last < 0) return 0;
    /* Allowed: only arithmetic and jumps; slots only written whole and as integers (and, if written,
       also read only that way: memory is then stale); segment accesses have an unknown base */
    for (i = 0; i <= last; i++) {
        const Insn *x = &b[i];
        int sz, l, s, kk;
        if (interp[i] || (traced[i] && traced[i] != 1)) return 0;
        if (kinds[i] != CONT && kinds[i] != JUMP_COND && kinds[i] != JUMP) return 0;
        if (kinds[i] == JUMP_COND) {       /* side exit back into the loop part (continue, inner loop):
                                               the loop would run through the block start, every iteration would pay
                                               for entry and exit */
            uint32_t t = x->eip + x->len + x->imm;
            if (t != eip) for (j = 0; j <= last; j++) if (b[j].eip == t) return 0;
        }
        if (!mem_access(x, &sz, &l, &s)) continue;
        if (!is_slot(x, base)) {
            if (x->seg == 0x64 || x->seg == 0x65) return 0;
            foreign_s |= s;
            continue;
        }
        kk = slot_k(x, base);
        for (j = 0; j < g_sl_n; j++)
            if ((int32_t)x->disp < g_sl_d[j] + 4 && (int32_t)x->disp + sz > g_sl_d[j] &&
                !(j == kk && sz == 4 && int_op(x) && x->opsz == 4)) {
                if (s) return 0;
                inexact |= 1 << j;
            }
        if (s && kk >= 0) wm |= 1 << kk;
    }
    if (inexact & wm) return 0;
    if (!wm && !foreign_s) return 0;          /* nothing to save */
    /* symbolically through the loop part: addresses of the other accesses, state at every backward jump */
    for (k = 0; k < 8; k++) r[k] = sym_mk(k, 0);
    for (k = 0; k < g_sl_n; k++) pl[k] = sym_mk(8 + k, 0);
    g_rp_n = 0;
    for (i = 0; i <= last; i++) {
        const Insn *x = &b[i];
        int sz, l, s;
        /* every other write access is checked, for written slots also every other read */
        if (mem_access(x, &sz, &l, &s) && !is_slot(x, base) && (s || wm)) {
            Sym sb = x->base >= 0 ? r[x->base] : sym_mk(SQ_K, 0), sx = x->index >= 0 ? r[x->index] : sym_mk(SQ_K, 0);
            RCheck *z;
            if (sb.q < 0 || sx.q < 0 || g_rp_n >= RP_MAX) return 0;
            if (sb.q == SQ_K && sx.q != SQ_K && !x->scale) { sb = sym_mk(sx.q, (int32_t)((uint32_t)sx.c + (uint32_t)sb.c)); sx = sym_mk(SQ_K, 0); }
            for (k = 0; k < g_rp_n; k++)    /* same address form: check once (larger access) */
                if (g_rp[k].qb == sb.q && g_rp[k].qx == sx.q && g_rp[k].s == (sx.q == SQ_K ? 0 : x->scale) &&
                    g_rp[k].c == (int32_t)((uint32_t)sb.c + ((uint32_t)sx.c << x->scale) + x->disp)) break;
            z = &g_rp[k];
            if (k == g_rp_n) {
                z->qb = sb.q; z->qx = sx.q; z->s = (int8_t)(sx.q == SQ_K ? 0 : x->scale); z->sz = 0; z->r = 0;
                z->c = (int32_t)((uint32_t)sb.c + ((uint32_t)sx.c << x->scale) + x->disp);
                g_rp_n++;
            }
            if (sz > z->sz) z->sz = (int8_t)sz;
        }
        sym_step(x, r, pl, base);
        if (is_ret(x, kinds[i], traced[i], eip)) {
            if (nk >= 8) return 0;
            memcpy(edge[nk], r, sizeof r);
            memcpy(edge[nk] + 8, pl, (size_t)g_sl_n * sizeof *pl);
            nk++;
        }
    }
    /* direction and largest step per access over all backward jumps */
    g_sl_budget = 0;
    for (k = 0; k < g_rp_n; k++) {
        RCheck *z = &g_rp[k];
        int trunc = 0, up = 0;
        z->m = 0;
        for (j = 0; j < nk; j++) {
            int32_t db, dx, dd;
            if (!sym_stride(edge[j], z->qb, &db) || !sym_stride(edge[j], z->qx, &dx)) return 0;
            dd = db + dx * (1 << z->s);
            if (dd <= -4096 || dd >= 4096) return 0;
            if (dd < 0) { trunc = 1; if (-dd > z->m) z->m = -dd; }
            if (dd > 0) { up = 1; if (dd > z->m) z->m = dd; }
        }
        z->r = (int8_t)(trunc && up ? 2 : trunc ? -1 : up ? 1 : 0);
        if (g_no_budget && z->r == 2) return 0;
        if (z->r && !g_no_budget) g_sl_budget = 1;
    }
    /* a budget needs backward jumps that can count along (conditional or unconditional in the loop part) */
    g_sl_wmask = wm;
    return 1;
}

/* At the loop entry (slots loaded): every checked access lies outside the slots; if it runs toward
   them, its distance limits the budget (r10, + 1 into CTR at the end). Failure branches into miss[] (return value:
   count). S1 = first slot - size (access below: address <= S1), E = past the last slot (above: >= E). */
static int ret_checkcode(uint32_t **miss)
{
    int k, nf = 0, lo = g_sl_d[0], hi = g_sl_d[0];
    for (k = 1; k < g_sl_n; k++) { if (g_sl_d[k] < lo) lo = g_sl_d[k]; if (g_sl_d[k] > hi) hi = g_sl_d[k]; }
    if (g_sl_budget) { e(LIS(10, 0x7fff)); e(ORI(10, 10, 0xffff)); }
    for (k = 0; k < g_rp_n; k++) {
        const RCheck *z = &g_rp[k];
        int rb = z->qb == SQ_K ? -1 : z->qb < 8 ? R(z->qb) : g_sl_reg[z->qb - 8];
        int rx = z->qx == SQ_K ? -1 : z->qx < 8 ? R(z->qx) : g_sl_reg[z->qx - 8];
        uint32_t *good[2], *share = 0;
        int ng = 0;
        if (rx >= 0 && z->s) { e(SLWI(12, rx, z->s)); rx = 12; }
        if (rb >= 0 && rx >= 0) { e(ADD(11, rb, rx)); addi_after(11, 11, z->c); }
        else if (rb >= 0 || rx >= 0) addi_after(11, rb >= 0 ? rb : rx, z->c);
        else load_imm(11, (uint32_t)z->c);
        switch (z->r) {
        case 0:                             /* fixed: below S1 or from E on */
            addi_after(12, R(g_sl_base), lo - z->sz);
            e(CMPLW(1, 11, 12));
            good[ng++] = p; e(BC_(BO_FALSE, 4 + CR_GT, 0));
            addi_after(12, R(g_sl_base), hi + 4);
            e(CMPLW(1, 11, 12));
            miss[nf++] = p; e(BC_(BO_TRUE, 4 + CR_LT, 0));
            break;
        case -1:                            /* down: free below, budget above (A - E) / m */
            addi_after(12, R(g_sl_base), lo - z->sz);
            e(CMPLW(1, 11, 12));
            if (!g_sl_budget) { miss[nf++] = p; e(BC_(BO_TRUE, 4 + CR_GT, 0)); break; }
            good[ng++] = p; e(BC_(BO_FALSE, 4 + CR_GT, 0));
            addi_after(12, R(g_sl_base), hi + 4);
            e(CMPLW(1, 11, 12));
            miss[nf++] = p; e(BC_(BO_TRUE, 4 + CR_LT, 0));
            e(SUBF(12, 12, 11));
            break;
        case 1:                             /* up: free above, budget below (S1 - A) / m */
            addi_after(12, R(g_sl_base), hi + 4);
            e(CMPLW(1, 11, 12));
            if (!g_sl_budget) { miss[nf++] = p; e(BC_(BO_TRUE, 4 + CR_LT, 0)); break; }
            good[ng++] = p; e(BC_(BO_FALSE, 4 + CR_LT, 0));
            addi_after(12, R(g_sl_base), lo - z->sz);
            e(CMPLW(1, 11, 12));
            miss[nf++] = p; e(BC_(BO_TRUE, 4 + CR_GT, 0));
            e(SUBF(12, 11, 12));
            break;
        default: {                          /* both directions: budget from the distance to the nearer side */
            uint32_t *top;
            addi_after(12, R(g_sl_base), lo - z->sz);
            e(CMPLW(1, 11, 12));
            top = p; e(BC_(BO_TRUE, 4 + CR_GT, 0));
            e(SUBF(12, 11, 12));
            share = p; e(B_(0));
            bc_set(top, p);
            addi_after(12, R(g_sl_base), hi + 4);
            e(CMPLW(1, 11, 12));
            miss[nf++] = p; e(BC_(BO_TRUE, 4 + CR_LT, 0));
            e(SUBF(12, 12, 11));
            *share = B_((uint32_t)(uintptr_t)p - (uint32_t)(uintptr_t)share);
        }
        }
        if (z->r && g_sl_budget) {          /* r12 = distance: iterations = distance / m, smallest into r10 */
            uint32_t *cont;
            if (!(z->m & (z->m - 1))) { int sh = 0; while ((1 << sh) < z->m) sh++; if (sh) e(SRWI(12, 12, sh)); }
            else { e(LI(9, z->m)); e(DIVWU(12, 12, 9)); }
            e(CMPLW(1, 12, 10));
            cont = p; e(BC_(BO_FALSE, 4 + CR_LT, 0));
            e(MR(10, 12));
            bc_set(cont, p);
        }
        while (ng) bc_set(good[--ng], p);
        g_st_ret_check++;
    }
    if (g_sl_budget) { e(ADDI(10, 10, 1)); e(MTCTR(10)); }
    return nf;
}

/* ---------- Slots stay dormant in inlined calls ----------
 * An inlined call (call/ret with stack frame push ebp; mov ebp,esp; ...; leave) no longer ends the slots ([ebp+d]):
 * during the call they are dormant (ebp belongs to the callee), afterwards they are valid again. The esp offset
 * is tracked; if the callee writes only to its own stack (push, [esp+d], [ebp_new+d]), the registers stay
 * valid, otherwise they are reloaded afterwards. Stack write accesses (also [esp+d] outside the calls,
 * arguments) all lie below esp_head + g_sl_smax; a check at entry (esp + smax <= ebp + lowest slot)
 * saves them the range check. */
#define PAUSES 4
static int g_sl_np, g_sl_pv[PAUSES], g_sl_pb[PAUSES], g_sl_pl[PAUSES];  /* dormant ranges [from, to], reload afterwards */
static int g_sl_espfree;                /* check at entry: stack write accesses below the slots */
static int32_t g_sl_smax;
static uint8_t g_sl_free[MAX_INSNS];  /* instruction i provably writes below the slots (no range check) */
static unsigned long g_st_pauses;

/* Dormant range from the inlined call c: index of the matching ret or -1. *laden: the callee may write
   somewhere else; *smax: highest end of its stack write accesses (offset to esp before the call) */
static int pause_verify(const Insn *b, const int *traced, const int *interp, int n, int c, int *has_load, int32_t *smax)
{
    int k, depth = 0, frame = 0;           /* frame: ebp points to the callee's stack frame */
    int32_t off = 0, ebp_c = 0;
    *has_load = 0;
    for (k = c; k < n; k++) {
        const Insn *x = &b[k];
        int op = x->op, sz, l, s;
        uint8_t m;
        if (interp[k]) return -1;
        if (traced[k] == 3) {             /* ret (inlined) */
            off += 4 + (op == 0xc2 ? (int32_t)(x->imm & 0xffff) : 0);
            if (depth-- == 0) return off == 0 && !frame ? k : -1;
            continue;
        }
        if (traced[k] == 2 || traced[k] == 4 || traced[k] == 8) {    /* call: return address */
            if (k > c) depth++;
            off -= 4;
            if (off + 4 > *smax) *smax = off + 4;
            continue;
        }
        if (traced[k] && traced[k] != 1) return -1;
        m = write_mask(x);
        if ((op >= 0x50 && op <= 0x57) || op == 0x68 || op == 0x6a) {
            off -= 4;
            if (off + 4 > *smax) *smax = off + 4;
            continue;
        }
        if (op >= 0x58 && op <= 0x5f) {     /* pop */
            if (op == 0x5c) return -1;
            off += 4;
            if (op == 0x5d) frame = 0;     /* pop ebp: caller's stack frame back */
            continue;
        }
        if (op == 0xc9) {                   /* leave */
            if (!frame) return -1;
            off = ebp_c + 4;
            frame = 0;
            continue;
        }
        if ((op == 0x89 || op == 0x8b) && !x->mem && x->opsz == 4 &&
            ((op == 0x89 && x->rm == EBP && x->reg == ESP) || (op == 0x8b && x->reg == EBP && x->rm == ESP))) {
            frame = 1; ebp_c = off;        /* mov ebp, esp */
            continue;
        }
        if ((op == 0x81 || op == 0x83) && !x->mem && x->opsz == 4 && x->rm == ESP && (x->reg == 0 || x->reg == 5)) {
            off += x->reg == 0 ? (int32_t)x->imm : -(int32_t)x->imm;
            continue;
        }
        if (m & ((1u << ESP) | (1u << EBP))) return -1;
        if (mem_access(x, &sz, &l, &s) && s) {
            if (x->mem && !x->seg && x->index < 0 && x->base == ESP) { if (off + (int32_t)x->disp + sz > *smax) *smax = off + (int32_t)x->disp + sz; }
            else if (x->mem && !x->seg && x->index < 0 && x->base == EBP && frame) {
                if (ebp_c + (int32_t)x->disp + sz > *smax) *smax = ebp_c + (int32_t)x->disp + sz;
            } else *has_load = 1;
        }
    }
    return -1;
}

/* choose slots for a loop block (sets g_sl_*); returns the first instruction from which they are no longer valid */
static int slots_choose(const Insn *b, const int *kinds, const int *traced, const int *interp, int n, uint32_t eip)
{
    static const int bases[2] = { EBP, ESP };
    int bi;
    g_sl_n = 0; g_sl_verify = 0; g_sl_active = 0; g_sl_wb = 0; g_sl_wmask = 0; g_rp_n = 0; g_sl_last = -1;
    g_sl_budget = 0; g_sl_np = 0; g_sl_espfree = 0;
    memset(g_sl_free, 0, (size_t)n);
    if (g_no_slots) return 0;
    for (bi = 0; bi < n && !is_ret(&b[bi], kinds[bi], traced[bi], eip); bi++) ;
    if (bi == n) return 0;                  /* no loop: nothing to choose (most blocks) */
    for (bi = 0; bi < 2; bi++) {
        int base = bases[bi], end = n, i, k, j, loop = 0, unknown = 0, nd = 0, ns = 0, last = -1;
        int unknown_loop = 0, gain, attempt, np = 0, pv[PAUSES], pb[PAUSES], pl[PAUSES], esp_fixed = 1, nfree = 0;
        int unknown_all = 0;
        int32_t d[MAX_INSNS], wa[MAX_INSNS], we[MAX_INSNS], lo, hi, smax = -0x7fffffff;
        int nread[MAX_INSNS], nwrite[MAX_INSNS], weight[MAX_INSNS], wfull[MAX_INSNS];
        uint8_t in_pause[MAX_INSNS], freemask[MAX_INSNS];
        int pauses_ok = base == EBP && !g_no_pause && !no_ret(eip);
        memset(in_pause, 0, (size_t)n);
        memset(freemask, 0, (size_t)n);
        for (i = 0; i < n; i++) {
            const Insn *x = &b[i];
            if (pauses_ok && esp_fixed && np < PAUSES && (traced[i] == 2 || traced[i] == 4 || traced[i] == 8)) {
                int has_load, r = pause_verify(b, traced, interp, n, i, &has_load, &smax);
                if (r > i) {
                    pv[np] = i; pb[np] = r; pl[np] = has_load; np++;
                    for (k = i; k <= r; k++) in_pause[k] = 1;
                    for (k = i; k <= r; k++) if (is_ret(&b[k], kinds[k], traced[k], eip)) loop = 1;
                    i = r;
                    continue;
                }
            }
            if (writes_reg(x, base) || is_push(x)) { end = i; break; }
            if (writes_reg(x, ESP)) esp_fixed = 0;
            if (is_ret(x, kinds[i], traced[i], eip)) loop = 1;
            if (pauses_ok && esp_fixed) {    /* write [esp+d] (arguments): below the slots, if the check holds */
                int sz, l, s;
                if (mem_access(x, &sz, &l, &s) && s && x->mem && !x->seg && x->index < 0 && x->base == ESP) {
                    freemask[i] = 1;
                    nfree++;
                    if ((int32_t)x->disp + sz > smax) smax = (int32_t)x->disp + sz;
                }
            }
        }
        /* reads in the loop (before the last backward jump) count more than after it */
        for (i = 0; i < end; i++)
            if (is_ret(&b[i], kinds[i], traced[i], eip)) last = i;
        for (i = 0; i < end; i++) {
            const Insn *x = &b[i];
            int sz, l, s;
            if (in_pause[i]) continue;      /* [ebp+d] belongs to the callee there */
            if (!mem_access(x, &sz, &l, &s)) continue;
            if (!is_slot(x, base)) {
                unknown_all += s;
                if (freemask[i]) continue;      /* stack below the slots (check at entry) */
                unknown += s; if (i <= last) unknown_loop += s;
                continue;
            }
            /* interpreted instructions reload the slots afterwards: their writes do no harm */
            if (s && !interp[i]) { wa[ns] = (int32_t)x->disp; we[ns] = (int32_t)x->disp + sz; wfull[ns] = sz == 4; ns++; }
            /* 8/16-bit reads from a slot rule it out as well: with deferred write-back, memory would
               still hold the old value (slot_reg knows only 32 bit) */
            else if (l && sz != 4 && !interp[i]) { wa[ns] = (int32_t)x->disp; we[ns] = (int32_t)x->disp + sz; wfull[ns] = 0; ns++; }
            if (sz != 4) continue;
            for (k = 0; k < nd && d[k] != (int32_t)x->disp; k++) ;
            if (k == nd) { d[nd] = (int32_t)x->disp; nread[nd] = 0; nwrite[nd] = 0; nd++; }
            nread[k] += l * (i <= last ? 16 : 1);
            if (i <= last) nwrite[k] += s * 16;
        }
        if (!loop || unknown > 32) continue;
        /* slots that are partially overwritten are ruled out */
        for (k = 0; k < nd; k++)
            for (j = 0; j < ns; j++)
                if (wa[j] < d[k] + 4 && we[j] > d[k] && !(wfull[j] && wa[j] == d[k])) nread[k] = nwrite[k] = 0;
        /* attempt 0: deferred write-back (then writes save too); attempt 1: immediately to memory */
        for (attempt = (g_no_ret || no_ret(eip)) ? 1 : 0; attempt < 2; attempt++) {
            for (k = 0; k < nd; k++) weight[k] = nread[k] + (attempt == 0 ? nwrite[k] : 0);
            g_sl_n = 0;
            gain = 0;
            /* with dormant calls at most g_pause_slots: the inlined call needs free r28..r31 for
               copies (arguments, saved ebp) */
            while (g_sl_n < (np && attempt == 1 ? g_pause_slots : SLOTS)) {     /* the most used ones */
                int best = -1;
                for (k = 0; k < nd; k++) if (weight[k] >= 16 && (best < 0 || weight[k] > weight[best])) best = k;
                if (best < 0) break;
                g_sl_d[g_sl_n++] = d[best];
                gain += weight[best] / 16;
                weight[best] = 0;
            }
            if (!g_sl_n) continue;
            /* write immediately: only if the slots save more loads than the checks cost */
            if (attempt == 1 && gain < unknown_loop) { g_sl_n = 0; continue; }
            lo = hi = g_sl_d[0];
            for (k = 1; k < g_sl_n; k++) { if (g_sl_d[k] < lo) lo = g_sl_d[k]; if (g_sl_d[k] > hi) hi = g_sl_d[k]; }
            g_sl_lo = lo - 15;                  /* accesses up to 16 bytes (SSE) */
            g_sl_len = hi + 4 - g_sl_lo;
            if (g_sl_len >= 32768) { g_sl_n = 0; continue; }
            if (attempt == 0 && (np || !ret_plan(b, kinds, traced, interp, eip, base, last))) {
                g_sl_n = 0; g_sl_budget = 0;
                continue;
            }
            g_sl_wb = attempt == 0;
            g_sl_last = g_sl_wb ? last : -1;
            g_sl_base = base;
            if (g_sl_wb || !(np || nfree)) g_sl_verify = unknown_all > 0;
            else {                          /* dormant calls, stack write accesses without range check */
                g_sl_np = np;
                for (k = 0; k < np; k++) { g_sl_pv[k] = pv[k]; g_sl_pb[k] = pb[k]; g_sl_pl[k] = pl[k]; }
                g_sl_espfree = 1;
                g_sl_smax = smax;
                memcpy(g_sl_free, freemask, (size_t)n);
                g_sl_verify = unknown > 0;
                g_st_pauses += (unsigned long)np;
            }
            g_st_slots += (unsigned long)g_sl_n;
            if (g_sl_wb) g_st_ret++;
            return end;
        }
        g_sl_n = 0;
    }
    return 0;
}

/* Choose constants for a loop block: immediates in the loop part that need two instructions or go to
   memory; the registers are those the stack slots leave free */
static void const_choose(const Insn *b, const int *kinds, const int *traced, int n, uint32_t eip)
{
    uint32_t w[MAX_INSNS], v = 0;
    int z[MAX_INSNS], nw = 0, i, k, last = -1;
    g_k_n = 0;
    if (g_no_slots) return;
    for (i = 0; i < n; i++)
        if ((kinds[i] == JUMP_COND || (kinds[i] == JUMP && !traced[i])) && b[i].eip + b[i].len + b[i].imm == eip)
            last = i;
    for (i = 0; i <= last; i++) {
        const Insn *x = &b[i];
        int op = x->op, counts = 0;
        if ((op < 0x40 && ((op & 7) == 4 || (op & 7) == 5)) || op == 0x80 || op == 0x81 || op == 0x83) {
            int g = op == 0x80 || (op < 0x40 && !(op & 1)) ? 1 : x->opsz;
            v = g == 1 ? x->imm & 0xff : g == 2 ? x->imm & 0xffff : x->imm;
            counts = (int32_t)v < -32768 || (int32_t)v > 32767;
        } else if (op == 0xc6 || op == 0xc7) {
            v = op == 0xc6 ? x->imm & 0xff : x->imm;
            counts = x->mem;
        } else if ((op >= 0xb8 && op <= 0xbf) || op == 0x69 || op == 0x68) {
            v = x->imm;
            counts = (int32_t)v < -32768 || (int32_t)v > 32767;
        }
        if (!counts) continue;
        for (k = 0; k < nw && w[k] != v; k++) ;
        if (k == nw) { w[nw] = v; z[nw] = 0; nw++; }
        z[k]++;
    }
    while (g_k_n < SLOTS - g_sl_n) {
        int best = -1;
        for (k = 0; k < nw; k++) if (z[k] > 0 && (best < 0 || z[k] > z[best])) best = k;
        if (best < 0) break;
        g_k_val[g_k_n] = w[best];
        g_k_reg[g_k_n] = g_sl_reg[g_sl_n + g_k_n];
        g_k_n++;
        z[best] = 0;
    }
}

/* Choose floating-point registers of a loop block: P = used in the loop, kept across the backward jump;
   T = first access is a full load from memory (before every exit), so dead at the head. Only if nothing in
   the loop empties the cache completely (interpreter, AltiVec integer, div). */
/* xmm accesses of a packed instruction (fp_loop_choose): l1, l2 read (low half as double), voll: entirely new */
static void pd_regs(const Insn *x, int *l1, int *l2, int *full)
{
    int o = x->op & 0xff, d = x->reg, s = x->mem ? -1 : x->rm;
    *l1 = *l2 = *full = -1;
    switch (o) {
    case 0x51: case 0xe6: case 0x7e: case 0x5a: *l2 = s; *full = d; return;
    case 0x10: case 0x11:                   /* movupd: load entirely new, store reads, between registers a copy */
        if (x->mem) { if (o == 0x10) *full = d; else *l1 = d; return; }
        if (o == 0x10) { *l2 = s; *full = d; } else { *l1 = d; *full = s; }
        return;
    case 0x13: case 0x17: case 0x50: *l1 = o == 0x50 ? s : d; return;
    case 0xd6: *l1 = d; if (!x->mem) *full = x->rm; return;
    }
    *l1 = d; *l2 = s;                       /* arithmetic, min/max, unpck, shufpd, cmppd, movlpd/movhpd load */
}

/* Choose floating-point registers of a loop block: P = used in the loop, kept across the backward jump;
   T = first access is a full load from memory (before every exit), so dead at the head. Only if nothing in
   the loop empties the cache completely (interpreter, AltiVec integer, div). Packed instructions: both halves
   as double; P registers that use them also hold their high half (g_fp_ph). */
static void fp_loop_choose(const Insn *b, const int *kinds, const int *traced, const int *interp, int n, uint32_t eip)
{
    int i, last = -1, exit_ptr = 0, pd = 0;
    char kind[8], first[8];
    uint8_t bad = 0, inuse = 0, pdm = 0;
#define FS_READ(r, a) do { int r_ = (r); char a_ = (a); if (r_ >= 0) { \
        if (!(inuse & (1 << r_))) { inuse |= (uint8_t)(1 << r_); kind[r_] = a_; first[r_] = 'L'; } \
        else if (kind[r_] != a_) bad |= (uint8_t)(1 << r_); } } while (0)
#define FS_FULL(r, a) do { int r_ = (r); char a_ = (a); if (r_ >= 0) { \
        if (!(inuse & (1 << r_))) { inuse |= (uint8_t)(1 << r_); kind[r_] = a_; first[r_] = exit_ptr ? 'L' : 'V'; } \
        else if (kind[r_] != a_) bad |= (uint8_t)(1 << r_); } } while (0)
    g_fp_p = g_fp_t = g_fp_ph = 0;
    if (g_no_slots) return;
    for (i = 0; i < n; i++)
        if ((kinds[i] == JUMP_COND || (kinds[i] == JUMP && !traced[i])) && b[i].eip + b[i].len + b[i].imm == eip)
            last = i;
    for (i = 0; i <= last; i++)
        if (b[i].op >= OP_0F && (pd_can(&b[i]) || (b[i].op == (OP_0F | 0x28) && b[i].p66))) pd = 1;
    for (i = 0; i <= last; i++) {
        const Insn *x = &b[i];
        int o, d, s, k;
        char a, ad, as;
        if (interp[i] || (x->op >= OP_0F && sse_can(x)) || (x->op == 0xf7 && x->reg >= 6)) return;
        if (kinds[i] == JUMP_COND) exit_ptr = 1;
        if (x->op >= OP_0F && pd_can(x)) {
            int l1, l2, v;
            pd_regs(x, &l1, &l2, &v);
            FS_READ(l1, 'D'); FS_READ(l2, 'D'); FS_FULL(v, 'D');
            if (l1 >= 0) pdm |= (uint8_t)(1 << l1);
            if (l2 >= 0) pdm |= (uint8_t)(1 << l2);
            if (v >= 0) pdm |= (uint8_t)(1 << v);
            continue;
        }
        if (x->op < OP_0F || !fp_can(x)) continue;
        o = x->op & 0xff; d = x->reg; s = x->mem ? -1 : x->rm;
        a = ad = as = x->rep == 0xf2 ? 'D' : 'F';
        if ((o == 0x28 || o == 0x29) && s >= 0) {   /* register copy: source read, destination entirely new (kind of the source) */
            int q = o == 0x28 ? s : d, z2 = o == 0x28 ? d : s;
            if (q == z2) continue;
            if (!(inuse & (1 << q))) {
                if (!pd) { bad |= (uint8_t)(1 << q) | (uint8_t)(1 << z2); continue; }
                FS_READ(q, 'D');           /* packed code: doubles */
            }
            if (pd) pdm |= (uint8_t)((1 << q) | (1 << z2));
            if (!(inuse & (1 << z2))) {
                inuse |= (uint8_t)(1 << z2); kind[z2] = kind[q]; first[z2] = exit_ptr ? 'L' : 'V';
            } else if (kind[z2] != kind[q]) bad |= (uint8_t)(1 << z2);
            continue;
        }
        if (o == 0x28 && x->p66 && pd) { FS_FULL(d, 'D'); pdm |= (uint8_t)(1 << d); continue; }    /* movapd load */
        if (o == 0x29 && x->p66 && pd) { FS_READ(d, 'D'); pdm |= (uint8_t)(1 << d); continue; }   /* movapd store */
        if (o == 0x57 && s == d) { FS_FULL(d, x->p66 ? 'D' : 'F'); continue; }    /* xorpd x, x: 0 */
        if (o >= 0x54 && o <= 0x57 && x->p66 && pd) {
            FS_READ(d, 'D'); FS_READ(s, 'D');
            pdm |= (uint8_t)(1 << d);
            if (s >= 0) pdm |= (uint8_t)(1 << s);
            continue;
        }
        switch (o) {
        case 0x28: case 0x29: case 0x54: case 0x55: case 0x56: case 0x57:
            bad |= (uint8_t)(1 << d);
            if (s >= 0) bad |= (uint8_t)(1 << s);
            continue;
        case 0x2e: case 0x2f: ad = as = x->p66 ? 'D' : 'F'; break;
        case 0x2a: s = -1; break;
        case 0x2c: case 0x2d: d = -1; break;
        case 0x5a: ad = a == 'D' ? 'F' : 'D'; break;
        case 0x11: k = d; d = s; s = k; break;          /* source is reg, destination rm (or memory) */
        }
        if (s >= 0) {
            if (!(inuse & (1 << s))) { inuse |= (uint8_t)(1 << s); kind[s] = as; first[s] = 'L'; }
            else if (kind[s] != as) bad |= (uint8_t)(1 << s);
        }
        if (d >= 0) {
            if (!(inuse & (1 << d))) {
                inuse |= (uint8_t)(1 << d);
                kind[d] = ad;
                first[d] = (o == 0x10 && x->mem && !exit_ptr) ? 'V' : 'L';
            } else if (kind[d] != ad) bad |= (uint8_t)(1 << d);
        }
    }
#undef FS_READ
#undef FS_FULL
    for (i = 0; i < 8; i++) {
        if (!(inuse & (1 << i)) || (bad & (1 << i))) continue;
        if (first[i] == 'V') g_fp_t |= (uint8_t)(1 << i);
        else {
            g_fp_p |= (uint8_t)(1 << i);
            g_fp_kind[i] = kind[i];
            if (g_xfh_on && kind[i] == 'D' && (pdm & (1 << i))) g_fp_ph |= (uint8_t)(1 << i);
        }
    }
}

/* Debugging: ROSETTA_FPFUSION=lo-hi (hex) merges floating-point compares only in this range */
static int fpf_allowed(uint32_t eip)
{
    static int was_read, n = 1;
    static uint32_t lo[2] = { 0, 1 }, hi[2] = { 0xffffffffu, 0 };
    if (!was_read) {
        const char *s = getenv("ROSETTA_FPFUSION");
        was_read = 1;
        if (s) n = sscanf(s, "%x-%x,%x-%x", &lo[0], &hi[0], &lo[1], &hi[1]) / 2;
    }
    return (eip >= lo[0] && eip <= hi[0]) || (n > 1 && eip >= lo[1] && eip <= hi[1]);
}

/* Debugging: ROSETTA_CONST=lo-hi (hex) predicts constants only in blocks from this range */
static int const_allowed(uint32_t eip)
{
    static int was_read;
    static uint32_t lo = 0, hi = 0xffffffffu;
    if (!was_read) {
        const char *s = getenv("ROSETTA_CONST");
        was_read = 1;
        if (s) sscanf(s, "%x-%x", &lo, &hi);
    }
    return eip >= lo && eip <= hi;
}

/* ---------- Translating a block ---------- */

typedef struct { uint32_t *bc; uint32_t target, target2, *head, loc, before; int kind, sz, put, indirect, sl_ret, inner, sl_load; XfState xf[8]; X7State x7; } Page;

static unsigned long g_st_math_direct, g_st_jumptab;

/* switch jump table (gcc, PIC): mov R,[T + X*4 + d]; add R,B; jmp *R as the last three instructions of the block.
   k = index of the mov; x, t, bb = registers X, T, B. The block gets its own table with host addresses. */
static int jumptab_pattern(const Insn *b, int j, int *k, int *x, int *t, int *bb)
{
    const Insn *a, *m;
    int r;
    if (j < 2 || b[j].op != 0xff || b[j].reg != 4 || b[j].mem) return 0;
    r = b[j].rm;
    a = &b[j - 1];
    m = &b[j - 2];
    if (a->mem || a->opsz != 4 || a->seg) return 0;
    if (a->op == 0x01 && a->rm == r) *bb = a->reg;
    else if (a->op == 0x03 && a->reg == r) *bb = a->rm;
    else return 0;
    if (*bb == r || m->op != 0x8b || !m->mem || m->opsz != 4 || m->seg || m->reg != r || m->base < 0 || m->index < 0 ||
        m->scale != 2 || m->base == r) return 0;
    *k = j - 2;
    *x = m->index;
    *t = m->base;
    return 1;
}

/* Intel target of the switch from index r6, table T + d and base B into r3 and into guest register r (fallback paths) */
static void jumptab_target(int r, int t, int bb, int32_t d)
{
    e(RLWINM(11, 6, 2, 0, 29));
    e(ADD(11, 11, R(t)));
    load_imm(0, (uint32_t)d);
    e(LWBRX(3, 11, 0));
    e(ADD(3, 3, R(bb)));
    e(MR(R(r), 3));
}

/* Dispatch through the block's own table (index in r6, saved by the mov); data area: T0, B0, 64 host addresses.
   The mov and add before it are dropped on the fast path (eax then holds only the jump address) */
static void jumptab_code(int r, int t, int bb, int32_t d)
{
    uint32_t *lis, *b_t, *b_b, *b_x, *b_empty, *to_check, *b_init, *data;
    int k;
    lis = p; e(0); e(0);                    /* lis/ori r12, data (filled in below) */
    e(LWZ(0, 0, 12)); e(CMPLW(7, R(t), 0)); b_t = p; e(BC_(BO_FALSE, 4 * 7 + CR_EQ, 0));
    to_check = p;
    e(LWZ(0, 4, 12)); e(CMPLW(7, R(bb), 0)); b_b = p; e(BC_(BO_FALSE, 4 * 7 + CR_EQ, 0));
    e(CMPLWI(7, 6, 63)); b_x = p; e(BC_(BO_TRUE, 4 * 7 + CR_GT, 0));
    e(RLWINM(11, 6, 2, 0, 29)); e(ADDI(11, 11, 8)); e(LWZX(0, 12, 11));
    e(CMPWI(7, 0, 0)); b_empty = p; e(BC_(BO_TRUE, 4 * 7 + CR_EQ, 0));
    e(MTCTR(0)); e(BCTR);
    bc_set(b_t, p);                      /* different table base: take it over the first time (T0 = 0) */
    e(CMPWI(7, 0, 0)); b_init = p; e(BC_(BO_FALSE, 4 * 7 + CR_EQ, 0));
    e(STW(R(t), 0, 12)); e(STW(R(bb), 4, 12));
    e(B_((uint32_t)(uintptr_t)to_check - (uint32_t)(uintptr_t)p));
    bc_set(b_empty, p);                   /* entry empty: the dispatcher translates the target and enters it */
    e(ADD(4, 12, 11)); e(ORI(4, 4, 2));
    e(STW(4, OFF(sse_tmp), RCPU));          /* save r4 (jumptab_target needs r11, r0, r3) */
    jumptab_target(r, t, bb, d);
    e(LWZ(4, OFF(sse_tmp), RCPU));
    b_to(exit_near(p));
    bc_set(b_init, p);                   /* different table or base: as before */
    bc_set(b_b, p);
    bc_set(b_x, p);
    jumptab_target(r, t, bb, d);
    indirect();
    data = p;
    for (k = 0; k < 2 + 64; k++) e(0);
    lis[0] = LIS(12, (uint32_t)(uintptr_t)data >> 16);
    lis[1] = ORI(12, 12, (uint32_t)(uintptr_t)data & 0xffff);
    g_st_jumptab++;
}
static unsigned long g_st_embed, g_st_before, g_st_before_miss;

/* Predict the target of the indirect call b[i] at translation time: address registers unchanged by the
   instructions collected so far (then their value in g_cpu holds), memory readable; *target = value now. A guard
   checks at run time. */
/* 32-bit word at adr, if readable */
/* Whether n bytes from a are really readable, and copy them. Only at translation time (predictions from register
   values can point anywhere; vm_region reports the shared libraries of the host at 0x90000000 as readable even
   though they contain holes without pages). A probe: copy directly; if that faults, the fault handler (main.c
   memfault, jit_probe_fault) jumps back here. vm_read_overwrite did the same safely, but as a Mach call of ~12 us:
   62,000 of them in a first start of Calculator, the second start (everything is translated) more */
static jmp_buf g_probe_jb;
static volatile int g_probing;
static void *volatile g_probe_thread;

int jit_probe_fault(void)
{
    if (!g_probing || g_probe_thread != (void *)pthread_self()) return 0;
    g_probing = 0;
    _longjmp(g_probe_jb, 1);
    return 1;
}

static int safe_read(uint32_t a, void *target, uint32_t n)
{
    if (!n || a + n - 1 < a) return 0;
    if (g_no_probe) {
        vm_size_t was_read = 0;
        return vm_read_overwrite(mach_task_self(), a, n, (vm_address_t)(uintptr_t)target, &was_read) == KERN_SUCCESS && was_read == n;
    }
    if (_setjmp(g_probe_jb)) {             /* faulted: the host still blocks the signal (no sigreturn) */
        sigset_t m;
        sigemptyset(&m);
        sigaddset(&m, SIGSEGV);
        sigaddset(&m, SIGBUS);
        sigprocmask(SIG_UNBLOCK, &m, 0);
        g_st_safe_read++;
        return 0;
    }
    g_probe_thread = (void *)pthread_self();
    g_probing = 1;
    memcpy(target, (const void *)(uintptr_t)a, n);
    g_probing = 0;
    return 1;
}

/* 16 bytes of code from a really present (predicted targets) */
static int code_present(uint32_t a) { uint8_t t[16]; return safe_read(a, t, 16); }

/* guest word at a (little endian), if readable: for the translator's own looks at guest memory (sys.c) */
int jit_safe_read32(uint32_t a, uint32_t *v)
{
    uint8_t b[4];
    if (!safe_read(a, b, 4)) return 0;
    *v = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return 1;
}

static int word_read(uint32_t addr, uint32_t *w)
{
    int s = page_prot(addr);
    uint32_t raw;
    if (s < 0 || !(s & VM_PROT_READ) || page_prot(addr + 3) < 0) return 0;
    if (!safe_read(addr, &raw, 4)) return 0;
    *w = swap32(raw);
    return 1;
}

/* Registers on reaching b[i], precomputed from the entry state (g_cpu): simple integer instructions, loads
   from readable memory (current state). Only for predictions (a guard checks at run time). known: bit mask */
static void precompute(const Insn *b, int i, uint32_t *r, uint8_t *known)
{
    int j, k;
    for (k = 0; k < 8; k++) r[k] = g_cpu->r[k];
    *known = 0xff;
    for (j = 0; j < i; j++) {
        const Insn *y = &b[j];
        uint8_t m = write_mask(y);
        int op = y->op, ok = 0, z = -1;
        uint32_t v = 0;
        if (!m) continue;
#define BK(q) ((*known >> (q)) & 1)
        if (y->opsz == 4 && !y->seg) switch (op) {
        case 0x89: if (!y->mem && BK(y->reg)) { z = y->rm; v = r[y->reg]; ok = 1; } break;
        case 0x8b:
            z = y->reg;
            if (!y->mem) { if (BK(y->rm)) { v = r[y->rm]; ok = 1; } }
            else if ((y->base < 0 || BK(y->base)) && (y->index < 0 || BK(y->index)))
                ok = word_read(y->disp + (y->base >= 0 ? r[y->base] : 0) + (y->index >= 0 ? r[y->index] << y->scale : 0), &v);
            break;
        case 0x8d:
            z = y->reg;
            if ((y->base < 0 || BK(y->base)) && (y->index < 0 || BK(y->index))) {
                v = y->disp + (y->base >= 0 ? r[y->base] : 0) + (y->index >= 0 ? r[y->index] << y->scale : 0);
                ok = 1;
            }
            break;
        case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
            z = op & 7; v = y->imm; ok = 1; break;
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f:
            z = op & 7; if (BK(z)) { v = r[z] + (op < 0x48 ? 1u : 0xffffffffu); ok = 1; } break;
        case 0x81: case 0x83: case 0xc1:
            if (y->mem) break;
            z = y->rm;
            if (!BK(z)) break;
            ok = 1;
            if (op == 0xc1) { if (y->reg == 4) v = r[z] << (y->imm & 31); else if (y->reg == 5) v = r[z] >> (y->imm & 31); else ok = 0; }
            else switch (y->reg) {
                case 0: v = r[z] + y->imm; break;
                case 1: v = r[z] | y->imm; break;
                case 4: v = r[z] & y->imm; break;
                case 5: v = r[z] - y->imm; break;
                case 6: v = r[z] ^ y->imm; break;
                default: ok = 0;
            }
            break;
        case 0x01: case 0x09: case 0x21: case 0x29: case 0x31:
            if (y->mem) break;
            z = y->rm;
            if (!BK(z) || !BK(y->reg)) break;
            ok = 1;
            v = op == 0x01 ? r[z] + r[y->reg] : op == 0x09 ? r[z] | r[y->reg] : op == 0x21 ? r[z] & r[y->reg] :
                op == 0x29 ? r[z] - r[y->reg] : r[z] ^ r[y->reg];
            break;
        }
#undef BK
        for (k = 0; k < 8; k++) if (m >> k & 1) *known &= (uint8_t)~(1u << k);
        if (ok && z >= 0) { r[z] = v; *known |= (uint8_t)(1u << z); }
    }
}

/* loop: the call site lies in a loop of the block (only then is precomputing worth it; outside, it cost
   4 % at startup because much more gets inlined) */
static int target_predict(const Insn *b, int i, uint32_t *target, int loop)
{
    const Insn *x = &b[i];
    uint32_t r[8];
    uint8_t known;
    if (!g_cpu || x->seg) return 0;
    precompute(b, i, r, &known);
    if (g_no_precompute || !loop) {   /* as before: only registers the block does not change up to here */
        int j;
        for (j = 0; j < i; j++) known &= (uint8_t)~write_mask(&b[j]);
        for (j = 0; j < 8; j++) r[j] = g_cpu->r[j];
    }
    if (!x->mem) { if (!(known >> x->rm & 1)) return 0; *target = r[x->rm]; return 1; }
    if ((x->base >= 0 && !(known >> x->base & 1)) || (x->index >= 0 && !(known >> x->index & 1))) return 0;
    return word_read(x->disp + (x->base >= 0 ? r[x->base] : 0) + (x->index >= 0 ? r[x->index] << x->scale : 0), target);
}

/* Inlining the function at z is worth it if its ret comes after at most max instructions (conditional jumps
   fall through, unconditional forward jumps are followed) and no further call, indirect jump or instruction the
   block cannot execute lies in between. So not import stubs (jmp *pointer): their target would otherwise return
   without a shadow entry. */
static int embed_worth_calc(uint32_t z, int max);
static int embed_worth(uint32_t z, int max)     /* result remembered per target (same functions, many callers) */
{
    uint32_t h = (z * 2654435761u) >> 22;
    int w;
    if (max == 24 && g_el_eip[h] == z) return g_el_w[h];
    w = embed_worth_calc(z, max);
    if (max == 24) { g_el_eip[h] = z; g_el_w[h] = (uint8_t)w; }
    return w;
}

static int embed_worth_calc(uint32_t z, int max)
{
    Insn t;
    uint8_t l, s;
    int kind, n;
    for (n = 0; n < max; n++) {
        /* readable and fixed (also the end of an instruction up to 15 bytes long): targets also come from predictions */
        if (z - COMMPAGE_BASE < COMMPAGE_SIZE || !page_readonly(z) || !page_readonly(z + 15) ||
            !code_present(z) || !decode(z, &t)) return 0;
        if (t.op == 0xe8 && t.imm == 0) { z = t.eip + t.len; continue; }  /* PIC: becomes push */
        if (!classify(&t, &l, &s, &kind)) { if (!inline_possible(&t)) return 0; kind = CONT; }
        if (kind == RET) return 1;
        if (kind == CALL && t.op == 0xe8 && !g_no_native) {   /* math call through an import stub is generated directly */
            Stub fn = thunk_stub(t.eip + t.len + t.imm);
            void *native;
            if (fn && thunk_kind(fn, &native) != 'g' && native) { z = t.eip + t.len; continue; }
        }
        if (kind == CALL || kind == CALL_IND || kind == JUMP_IND) return 0;
        z = t.eip + t.len;
        if (kind == JUMP) { if ((int32_t)t.imm < 0) return 0; z += t.imm; }
    }
    return 0;
}

/* Whether the code from a (after a call) soon leads back to the block start. Only then is continuing in the block
   worth it (otherwise a lot of code would be translated that may never run after the call: startup of hello +15 %) */
static int loop_behind_n(uint32_t a, uint32_t eip, int n)
{
    int k;
    for (k = 0; k < n; k++) {
        Insn t;
        uint8_t l, s;
        int kind;
        if (a - COMMPAGE_BASE < COMMPAGE_SIZE || !page_readonly(a) || !page_readonly(a + 15) ||
            !code_present(a) || !decode(a, &t)) return 0;
        if (!classify(&t, &l, &s, &kind)) kind = CONT;
        if ((kind == JUMP_COND || kind == JUMP) && t.eip + t.len + t.imm == eip) return 1;
        if (kind != CONT && kind != JUMP_COND) return 0;
        a = t.eip + t.len;
    }
    return 0;
}

static int loop_behind(uint32_t a, uint32_t eip) { return loop_behind_n(a, eip, 20); }

/* Does the block's own path from a (conditional jumps fall through, unconditional ones forward are followed, as the
   collector does) arrive at t within n instructions, or jump back to the block start eip? */
static int path_reaches(uint32_t a, uint32_t t, uint32_t eip, int n)
{
    int k;
    for (k = 0; k < n; k++) {
        Insn y;
        uint8_t l, s;
        int kind;
        uint32_t z;
        if (a == t || a == eip) return 1;
        if (a - COMMPAGE_BASE < COMMPAGE_SIZE || !page_readonly(a) || !page_readonly(a + 15) ||
            !code_present(a) || !decode(a, &y)) return 0;
        if (!classify(&y, &l, &s, &kind)) kind = CONT;
        z = y.eip + y.len + y.imm;
        if ((kind == JUMP_COND || kind == JUMP) && z == eip) return 1;
        if (kind == JUMP) { if (z <= y.eip) return 0; a = z; continue; }   /* backward: the collector stops there */
        if (kind != CONT && kind != JUMP_COND) return 0;
        a = y.eip + y.len;
    }
    return 0;
}

/* After a call with continuation in the block: the called code has used flags, floating-point, constant and
   copy registers; the block no longer knows esp/ebp for certain (ret n, calling convention) */
static void after_call(void)
{
    int k;
    fs.kind = -1; fs.stored = 1;
    fw_flush();
    sw_all_drop();
    g_sw_esp_ok = g_sw_ebp_ok = 0;
    g_sw_ebp_own = 0;
    g_xm_ok = 0;
    for (k = 0; k < g_k_n; k++) load_imm(g_k_reg[k], g_k_val[k]);
    g_st_cont++;
}

/* Function of the Intel libSystem with a native version (thunk.c): call the stub, then ret as translated.
   The flags (r24..r27) and ebx/esi/edi/ebp stay, the C code leaves non-volatile registers alone. */
static unsigned long g_st_stubs, g_st_native;

/* Commpage routines with arguments on the stack as stub blocks: before, every call (memcpy from the
   libSystem jumps in with jmp *eax) went through the table, dispatcher and step() - sed called memcpy 60000 times */
static uint32_t cp_arg(Cpu *c, int i) { return ld32(c->r[ESP] + 4 + 4 * (uint32_t)i); }
static int cp_bzero(Cpu *c)
{
    memset((void *)(uintptr_t)cp_arg(c, 0), 0, cp_arg(c, 1));
    c->r[EAX] = cp_arg(c, 0);
    return 0;
}
static int cp_bcopy(Cpu *c)
{
    memmove((void *)(uintptr_t)cp_arg(c, 1), (const void *)(uintptr_t)cp_arg(c, 0), cp_arg(c, 2));
    c->r[EAX] = cp_arg(c, 1);
    return 0;
}
static int cp_memcpy(Cpu *c)
{
    memmove((void *)(uintptr_t)cp_arg(c, 0), (const void *)(uintptr_t)cp_arg(c, 1), cp_arg(c, 2));
    c->r[EAX] = cp_arg(c, 0);
    return 0;
}
/* Commpage routines with register arguments (commpage.c): without a stub every call went through dispatcher and
   interpreter, instruction by instruction (OSAtomicCompareAndSwap32 ~1000 times during Foundation startup). The
   guest has no threads: atomic here means read, modify, write. Flags like the x86 instruction (lazy: cmp/add, bt via
   CF), the block does the ret */
static void cp_cmp(Cpu *c, uint32_t a, uint32_t b)       /* flags like cmp a, b (32 bit) */
{
    c->cc_op = CC_SUB; c->cc_size = 4; c->cc_a = a; c->cc_b = b; c->cc_res = a - b;
}
static int cp_cas32(Cpu *c)             /* lock cmpxchg %edx,(%ecx) */
{
    uint32_t m = ld32(c->r[ECX]);
    cp_cmp(c, c->r[EAX], m);
    if (m == c->r[EAX]) st32(c->r[ECX], c->r[EDX]); else c->r[EAX] = m;
    return 0;
}
static int cp_cas64(Cpu *c)             /* lock cmpxchg8b (%esi): ZF only */
{
    uint64_t m = ld64(c->r[ESI]), prev = (uint64_t)c->r[EDX] << 32 | c->r[EAX];
    uint32_t fl = flags_fetch(c) & ~F_ZF;
    if (m == prev) { st64(c->r[ESI], (uint64_t)c->r[ECX] << 32 | c->r[EBX]); fl |= F_ZF; }
    else { c->r[EAX] = (uint32_t)m; c->r[EDX] = (uint32_t)(m >> 32); }
    flags_set(c, fl);
    return 0;
}
static int cp_xadd(Cpu *c)              /* lock xaddl %eax,(%edx) */
{
    uint32_t m = ld32(c->r[EDX]), v = c->r[EAX];
    st32(c->r[EDX], m + v);
    c->r[EAX] = m;
    c->cc_op = CC_ADD; c->cc_size = 4; c->cc_a = m; c->cc_b = v; c->cc_res = m + v;
    return 0;
}
static int cp_bit(Cpu *c, int do_set)   /* lock btsl/btrl %eax,(%edx): signed bit number, CF = old bit */
{
    uint32_t a = c->r[EDX] + (uint32_t)(((int32_t)c->r[EAX] >> 5) * 4), m = ld32(a), bit = 1u << (c->r[EAX] & 31);
    uint32_t fl = flags_fetch(c) & ~F_CF;
    if (m & bit) fl |= F_CF;
    st32(a, do_set ? m | bit : m & ~bit);
    flags_set(c, fl);
    return 0;
}
static int cp_bts(Cpu *c) { return cp_bit(c, 1); }
static int cp_btr(Cpu *c) { return cp_bit(c, 0); }
static int cp_spin_try(Cpu *c)          /* spin_lock_try(p): 1 if it was free (then taken) */
{
    uint32_t p = cp_arg(c, 0), m = ld32(p);
    cp_cmp(c, 0, m);
    if (!m) st32(p, 1);
    c->r[EDX] = p; c->r[ECX] = 1; c->r[EAX] = !m;
    return 0;
}
static int cp_spin_lock(Cpu *c)         /* spin_lock(p): without threads the lock is free (otherwise the guest would hang forever) */
{
    uint32_t p = cp_arg(c, 0);
    /* with threads another one holds it: change nothing, 1 = block goes to the dispatcher (eip stays here), which
       first lets other threads run and then calls again */
    if (g_threads > 1 && ld32(p)) { c->yield = 1; return 1; }
    cp_cmp(c, 0, ld32(p));
    st32(p, 1);
    c->r[EDX] = p; c->r[ECX] = 1; c->r[EAX] = 0;
    return 0;
}
static int cp_spin_unlock(Cpu *c) { c->r[EDX] = cp_arg(c, 0); st32(c->r[EDX], 0); return 0; }
static int cp_nothing(Cpu *c) { (void)c; return 0; }      /* memory barrier: only ret */
static int cp_gtod(Cpu *c) { c->r[EAX] = 1; return 0; }   /* gettimeofday: 1 = libSystem makes the system call */

/* stub reads or writes guest registers other than eax/esp: the block stores them all before and reloads them after */
static int stub_all_register(Stub fn)
{
    return fn == cp_cas32 || fn == cp_cas64 || fn == cp_xadd || fn == cp_bts || fn == cp_btr || fn == cp_spin_try ||
           fn == cp_spin_lock || fn == cp_spin_unlock;
}

static int g_no_cp_stub = -1;       /* ROSETTA_NO_CP_STUB: commpage through the dispatcher as before */
static Stub commpage_stub(uint32_t eip)
{
    if (g_no_cp_stub < 0) g_no_cp_stub = getenv("ROSETTA_NO_CP_STUB") != 0;
    if (g_no_cp_stub) return 0;
    switch (eip - COMMPAGE_BASE) {
    case 0x600: return cp_bzero;
    case 0x780: return cp_bcopy;
    case 0x7a0: return cp_memcpy;
    case 0x1800: return cp_cas32;
    case 0x1810: return cp_cas64;
    case 0x1820: return cp_xadd;
    case 0x1830: return cp_bts;
    case 0x1840: return cp_btr;
    case 0x220: return cp_spin_try;
    case 0x260: return cp_spin_lock;
    case 0x2a0: return cp_spin_unlock;
    case 0x180: case 0x4e0: case 0x520: return cp_nothing;
    case 0x2e0: return cp_gtod;
    }
    return 0;
}

static uint32_t *stub_block(uint32_t eip, Stub fn)
{
    uint32_t *start;
    room(8192);
    g_sl_active = 0; g_sl_n = 0; g_k_n = 0; g_fp_p = g_fp_t = g_fp_ph = 0; g_sl_wb = g_sl_wb_now = g_sl_budget = 0; g_xm_ok = 0;
    g_xfh_on = 0;
    fs.kind = -1; fs.sz = 4; fs.stored = 1;
    memset(g_xf, 0, sizeof g_xf);
    x7_empty();
    p = g_free;
    start = p;
    {
        void *native;
        char kind = thunk_kind(fn, &native);
        if (kind != 'g' && native && !g_no_native) {
            /* direct math function: arguments byte-swapped via AltiVec into f1/f2, call the PowerPC function, f1 to st(0) */
            char ga = (kind == '1' || kind == '2') ? 'D' : 'F';
            int two = kind == '2' || kind == 'F', z1 = (int)OFF(fp_store), z2 = z1 + 16;
            e(ADDI(12, R(ESP), 4));
            vswap_put(12, ga, z1);
            if (two) { e(ADDI(12, R(ESP), ga == 'D' ? 12 : 8)); vswap_put(12, ga, z2); }
            e(ga == 'D' ? LFD(1, z1, RCPU) : LFS(1, z1, RCPU));
            if (two) e(ga == 'D' ? LFD(2, z2, RCPU) : LFS(2, z2, RCPU));
            c_call(native);
            e(LWZ(11, OFF(st_top), RCPU));     /* push st(0) = f1 */
            e(ADDI(11, 11, -1));
            e(RLWINM(11, 11, 0, 29, 31));
            e(STW(11, OFF(st_top), RCPU));
            e(RLWINM(12, 11, 3, 26, 28));
            e(ADD(12, 12, RCPU));
            e(STFD(1, OFF(st), 12));
            e(LWZ(4, OFF(x87_depth), RCPU));
            e(CMPLWI(1, 4, 8));
            e(BC_(BO_TRUE, 4 + CR_EQ, 8));      /* depth at most 8 */
            e(ADDI(4, 4, 1));
            e(STW(4, OFF(x87_depth), RCPU));
            g_st_native++;
        } else {
            int all = stub_all_register(fn), i;
            if (all) { for (i = 0; i < 8; i++) e(STW(R(i), OFF(r[i]), RCPU)); }
            else e(STW(R(ESP), OFF(r[ESP]), RCPU));
            e(MR(3, RCPU));
            c_call((void *)fn);
            if (fn == cp_spin_lock) {       /* taken (another thread): to the dispatcher, back here later */
                uint32_t *cont;
                e(CMPWI(0, 3, 0));
                cont = p;
                e(0);
                exit_kind(eip, KIND_NORMAL);
                *cont = BC_(BO_TRUE, CR_EQ, (uint32_t)(uintptr_t)p - (uint32_t)(uintptr_t)cont);
            }
            if (all) { for (i = 0; i < 8; i++) if (i != ESP) e(LWZ(R(i), OFF(r[i]), RCPU)); }
            else e(LWZ(R(EAX), OFF(r[EAX]), RCPU));
        }
    }
    e(LWBRX(3, 0, R(ESP)));
    e(ADDI(R(ESP), R(ESP), 4));
    shadow_back();
    indirect();
    g_free = p;
    sys_icache_invalidate(start, (size_t)((uint8_t *)p - (uint8_t *)start));
    map_add(eip, start);
    page_note(eip);
    {
        uint32_t sp[DEP_SPANS] = { 0 };
        dep_note(sp, eip);
        dep_add(eip, start, sp);
    }
    g_st_stubs++;
    if (g_vt_on) fprintf(stderr, "rosetta: stub %s at %08x\n", thunk_name(fn), eip);
    return start;
}

static uint32_t *translate_block(uint32_t eip);

/* translation with timing (ROSETTA_STATS): how much of the startup the translation itself costs */
static uint64_t g_trans_time;
static int g_trans_depth;
static uint64_t g_time_phase[4], g_time_mark;  /* collecting, analyses, generation, exits and rest */

/* page faults of the process so far (ROSETTA_STATS: attribution to phases) */
int jit_pagefault(void)
{
    task_events_info_data_t ev;
    mach_msg_type_number_t n = TASK_EVENTS_INFO_COUNT;
    memset(&ev, 0, sizeof ev);
    task_info(mach_task_self(), TASK_EVENTS_INFO, (task_info_t)&ev, &n);
    return ev.faults;
}

static int g_trans_error, g_create_error;
static uint32_t *g_free_loaded;        /* end of the code from the translation cache (before it: blocks from the translation cache) */
static unsigned long g_st_inval_cache;
static uint32_t g_inval_eip[1024];        /* translation cache blocks whose byte check has failed once already */

/* first failure for this eip: then 1 and recorded; table full: 0 */
static int once_invalid(uint32_t eip)
{
    uint32_t h = (eip * 2654435761u) >> 22, i;
    for (i = 0; i < 1024; i++, h = (h + 1) & 1023) {
        if (g_inval_eip[h] == eip) return 0;
        if (!g_inval_eip[h]) { g_inval_eip[h] = eip; return 1; }
    }
    return 0;
}
/* g_cache_show (above): ROSETTA_CACHE_SHOW=1 reasons, =2 also every newly translated block */

static uint32_t *translate(uint32_t eip)
{
    uint64_t t0;
    uint32_t *r;
    int f0;
    if (!g_vt_on || g_trans_depth) return translate_block(eip);
    g_trans_depth++;
    f0 = g_vt_on > 1 ? jit_pagefault() : 0;
    t0 = mach_absolute_time();
    r = translate_block(eip);
    if (g_cache_show > 1)
        fprintf(stderr, "rosetta: new %08x kind %u bytes %02x %02x %02x %s\n", eip, g_cpu ? g_cpu->jit_kind : 0, ld8(eip),
                ld8(eip + 1), ld8(eip + 2), page_writable(eip) ? "writable" : "fixed");
    g_trans_time += mach_absolute_time() - t0;
    if (g_vt_on > 1) g_trans_error += jit_pagefault() - f0;
    g_trans_depth--;
    return r;
}

static uint32_t *translate_block(uint32_t eip)
{
    Insn b[MAX_INSNS];
    uint8_t reads[MAX_INSNS], writes[MAX_INSNS], afterwards[MAX_INSNS], target_needs[MAX_INSNS];
    int kinds[MAX_INSNS], traced[MAX_INSNS], interp[MAX_INSNS];
    Page pages[2 * MAX_INSNS];          /* side exits (one more per backward jump with budget) */
    int n = 0, ns = 0, i, max = g_max_insns, must_check = 0, can_not = 0, end_kind = CONT, sl_end = 0;
    uint32_t a = eip, *start, *head, cval[MAX_INSNS], *watchdog_bc[8], *ret_miss[2 * RP_MAX];
    int is_loop = 0, head_wb = 0, head_wmask = 0;
    XfState head_xf[8];
    uint8_t live, end_live, watchdog = 0;
    int nrf = 0, has_inner = 0, last_eip = -1, ncont = 0, ncall = 0, npre = 0, pre_next = 0, nstub = 0;
    int pre_pos[MAX_INSNS], pre_slot[MAX_INSNS];    /* hoisting floating-point loads: issue site, slot */
    uint8_t pre_on[MAX_INSNS];
    uint32_t *call_bl[4], call_target[4];           /* calls with continuation: bl and target (0: indirect), exit stub at the end */
    int inner[MAX_INSNS];                     /* inner jump: index of the target instruction (-1: none) */
    uint8_t inner_target[MAX_INSNS], inner_fl[MAX_INSNS], inner_on[MAX_INSNS], inner_ret[MAX_INSNS];
    uint32_t *inner_code[MAX_INSNS];
    int inner_esp_ok[MAX_INSNS], inner_ebp_ok[MAX_INSNS];  /* backward target: esp/ebp state it relies on */
    int32_t inner_esp[MAX_INSNS], inner_ebp[MAX_INSNS];
    int nw = 0, stream[MAX_INSNS], stream_i[VL_STREAMS], vl_n = 0, vl_cont_i = -1, ndeep = 0;
    int32_t pre_d[VL_STREAMS];
    uint32_t expected[MAX_INSNS], ret[2];   /* inlined calls: expected return address per ret */
    Stub stub_fn[MAX_INSNS];                /* direct math (traced 5): stub behind the import stub */
    int st_k = -1, st_x = 0, st_t = 0, st_b = 0;   /* switch jump table */
    int ninv = 0, natt = 0;                 /* conditional jumps followed on their taken branch, checked for it */
    /* Loop head: in a variant (block from a call site for one fixed target) a jump back to
       the call site does not go to its own head, because the next target can be a different one */
    int var = g_var_active && eip == g_var_loc;
    uint32_t keip = var ? 0xffffffffu : eip;
    if (eip - COMMPAGE_BASE < COMMPAGE_SIZE) {
        Stub cp = var ? 0 : commpage_stub(eip);
        return cp ? stub_block(eip, cp) : 0;
    }
    if (g_vt_on) g_time_mark = mach_absolute_time();
    if (!var) {
        Stub fn = thunk_find(eip);
        if (fn) return stub_block(eip, fn);
    }
    room(8192);
    if (page_writable(eip)) { if (var) return 0; must_check = 1; max = 4; }

    /* collect instructions: conditional jumps fall through, unconditional ones to targets not yet seen are followed */
    while (n < max) {
        int k, j;
        if (!decode(a, &b[n])) { can_not = 1; break; }
        interp[n] = 0;
        /* call to the next instruction (PIC: call 1f; 1: pop ebx) never returns: as push, without shadow stack */
        if (b[n].op == 0xe8 && b[n].imm == 0 && !g_no_pic) { b[n].op = 0x68; b[n].imm = b[n].eip + b[n].len; b[n].wm_ok = 0; }
        if (!classify(&b[n], &reads[n], &writes[n], &kinds[n])) {
            /* not translatable: interpret in the block if it does not change the control flow */
            if (!inline_possible(&b[n])) { can_not = 1; break; }
            if (g_vt_on) fprintf(stderr, "rosetta: interpreted in block: %08x op %03x reg %d mod %d seg %02x lock %d rep %02x p66 %d\n",
                                 b[n].eip, b[n].op, b[n].reg, b[n].mod, b[n].seg, b[n].lock, b[n].rep, b[n].p66);
            flags_generic(&b[n], &reads[n], &writes[n], &kinds[n]);
            kinds[n] = CONT;
            interp[n] = 1;
        }
        traced[n] = 0;
        a = b[n].eip + b[n].len;
        k = kinds[n++];
        if (a == keip && !must_check && n < max && (k == CONT || k == JUMP_COND) && !g_no_close) {
            /* the path falls into its own start: the loop closes here (backward jump without its own instruction) */
            Insn *j = &b[n];
            memset(j, 0, sizeof *j);
            j->eip = a; j->op = 0xeb; j->opsz = 4; j->base = j->index = -1;
            reads[n] = writes[n] = 0; kinds[n] = JUMP; traced[n] = 0; interp[n] = 0;
            n++;
            g_st_closed++;
            break;
        }
        if (k == CONT) { if (must_check && a - eip > 12) break; continue; }
        if (must_check) break;
        if (k == JUMP_COND) {
            /* The loop runs over the taken branch: gcc puts the rarely taken part inline after the jump (gzip's
               longest_match: jne to the next chain entry, which jumps back to the loop head; the fall-through compares
               further bytes). If the taken branch leads back to the block start soon and the fall-through does not
               get there (nor to the target), follow the taken branch: the loop becomes one block with its back jump
               (slots, constants, no exit per iteration). The jump is inverted and points to the instruction after
               it, which is now the side exit. */
            Insn *y = &b[n - 1];
            uint32_t z = a + y->imm;
            /* (forward jumps only, 12 instructions, the first 6 per block: the lookahead decodes; without these
               limits bzip2's translation took 20 % longer) */
            if (!g_no_taken && !var && ninv < 2 && n < max - 8 && z > a && z != keip && natt < 6 &&
                ((y->op >= 0x70 && y->op <= 0x7f) || (y->op >= (OP_0F | 0x80) && y->op <= (OP_0F | 0x8f))) &&
                z - COMMPAGE_BASE >= COMMPAGE_SIZE && !page_writable(z)) {
                for (j = 0; j < n && b[j].eip != z; j++) ;
                if (j == n) natt++;
                if (j == n && loop_behind_n(z, keip, 12) && !path_reaches(a, z, keip, 100)) {
                    y->op ^= 1;             /* condition negated (Intel: bit 0 of the condition code) */
                    y->imm = 0;             /* target: the old fall-through */
                    y->wm_ok = 0;
                    ninv++;
                    g_st_taken++;
                    a = z;
                }
            }
            continue;
        }
        if (k == JUMP && n < max - 4) {
            uint32_t z = a + b[n - 1].imm;
            for (j = 0; j < n && b[j].eip != z; j++) ;
            if (j == n && z - COMMPAGE_BASE >= COMMPAGE_SIZE && !page_writable(z)) { traced[n - 1] = 1; a = z; continue; }
        }
        /* direct call: continue reading in the block (the return address still goes onto the Intel stack) */
        if (k == CALL && b[n - 1].op == 0xe8 && ndeep < 2 && n < max - 8 && !g_no_embed) {
            uint32_t z = a + b[n - 1].imm;
            if (z - COMMPAGE_BASE >= COMMPAGE_SIZE && !page_writable(z) && !thunk_find(z) &&
                embed_worth(z, 24)) {
                traced[n - 1] = 2;
                ret[ndeep++] = a;
                a = z;
                continue;
            }
        }
        /* call of an import stub whose pointer points to a math function with a PowerPC counterpart: call directly here */
        if (k == CALL && b[n - 1].op == 0xe8 && !g_no_embed && n < max - 4) {
            uint32_t z = a + b[n - 1].imm;
            Stub fn = thunk_stub(z);      /* import stub belongs (per the indirect symbol table) to a stub function */
            void *native;
            char kind;
            if (!fn && z - COMMPAGE_BASE >= COMMPAGE_SIZE) fn = thunk_find(z);   /* or directly to it */
            if (fn) {
                kind = thunk_kind(fn, &native);
                if (kind != 'g' && native && !g_no_native) {
                    traced[n - 1] = 5;
                    stub_fn[n - 1] = fn;
                    continue;               /* a = after the call */
                }
                /* string stub (the C wrapper reads the arguments from the Intel stack): also call it here, without
                   stub block, shadow stack and ret */
                if (kind == 'g' && !g_no_stub_direct) {
                    traced[n - 1] = 7;
                    stub_fn[n - 1] = fn;
                    nstub++;
                    continue;
                }
            }
        }
        /* variant: the call at its start certainly goes to its target (the call site has compared) */
        if (k == CALL_IND && var && n == 1) {
            traced[0] = 8;
            ret[ndeep++] = a;
            a = g_var_target;
            continue;
        }
        /* call through a pointer: predict the target now and inline it (a guard compares at run time) */
        if (k == CALL_IND && ndeep < 2 && n < max - 8 && !g_no_embed && !no_spec(eip) && !var) {
            uint32_t z;
            if (target_predict(b, n - 1, &z, loop_behind(a, keip)) && z - COMMPAGE_BASE >= COMMPAGE_SIZE && page_readonly(z) &&
                !thunk_find(z) && embed_worth(z, 24)) {
                if (g_vt_on) fprintf(stderr, "rosetta: call at %08x predicted -> %08x (block %08x)\n", b[n - 1].eip, z, eip);
                traced[n - 1] = 4;
                expected[n - 1] = z;        /* here: predicted target */
                ret[ndeep++] = a;
                a = z;
                continue;
            }
        }
        /* ret of the inlined function: continue after checking the address behind the call */
        if (k == RET && ndeep > 0) {
            traced[n - 1] = 3;
            expected[n - 1] = ret[--ndeep];
            a = expected[n - 1];
            continue;
        }
        /* Call that is not inlined: the block continues after it (the return comes via blr to the
           location after the bl); loops with calls thus stay one block */
        if ((k == CALL || k == CALL_IND) && !traced[n - 1] && n < max - 4 && ncont < 3 && g_cont &&
            (g_cont == 2 || loop_behind(a, keip))) {
            traced[n - 1] = 6;
            ncont++;
            continue;                       /* a = return address */
        }
        break;
    }
    if (n == 0) return 0;
    if (g_vt_on) { uint64_t t = mach_absolute_time(); g_time_phase[0] += t - g_time_mark; g_time_mark = t; }
    for (i = 0; i < n; i++)                 /* calling convention: the call leaves no live flags */
        if (traced[i] == 6 || traced[i] == 7) { if (g_careful & 1) reads[i] = TRAP; writes[i] = TRAP; }
    if (kinds[n - 1] != CONT && kinds[n - 1] != JUMP_COND && !traced[n - 1]) end_kind = kinds[n - 1];
    if (end_kind != JUMP_IND || must_check || g_no_jumptab || !jumptab_pattern(b, n - 1, &st_k, &st_x, &st_t, &st_b))
        st_k = -1;                          /* otherwise: switch jump table at the block end (mov at st_k) */

    /* flags that are still read after each instruction */
    switch (end_kind) {
    case JUMP: end_live = needed(b[n - 1].eip + b[n - 1].len + b[n - 1].imm); break;
    case CALL: case CALL_IND: end_live = (g_careful & 1) ? TRAP : 0; break;
    case RET: end_live = (g_careful & 2) ? TRAP : 0; break;
    case JUMP_IND: end_live = (g_careful & 4) ? TRAP : 0; break;   /* jump tables: targets read no flags */
    default: end_live = needed(a);
    }
    live = end_live;
    for (i = n - 1; i >= 0; i--) {
        target_needs[i] = 0;
        if (kinds[i] == JUMP_COND) {
            target_needs[i] = needed(b[i].eip + b[i].len + b[i].imm);
            live |= target_needs[i];
        }
        afterwards[i] = live;
        live = (uint8_t)((live & ~writes[i]) | reads[i]);
    }

    g_sl_active = 0;
    g_sl_n = 0;
    g_sl_wb = g_sl_wb_now = g_sl_budget = 0;
    g_k_n = 0;
    g_fp_p = g_fp_t = g_fp_ph = 0;
    g_xfh_on = !g_no_xfh;                 /* f24..f31 for packed high halves if no x87 instruction needs them */
    for (i = 0; i < n && g_xfh_on; i++) if (b[i].op >= 0xd8 && b[i].op <= 0xdf) g_xfh_on = 0;
    if (!must_check) {
        sl_end = slots_choose(b, kinds, traced, interp, n, keip);
        /* block falls back into itself: this backward jump has not been analyzed, write slots immediately */
        if (g_sl_wb && end_kind == CONT && a == keip) { g_sl_wb = 0; g_sl_budget = 0; g_sl_last = -1; }
        const_choose(b, kinds, traced, n, keip);
        fp_loop_choose(b, kinds, traced, interp, n, keip);
    }
    /* Inner jumps: a conditional jump to a later instruction of this block jumps directly into its code instead of
       leaving the block (if/else, return path of an inlined function). No calls in between; not
       from the loop part with deferred write-back (the straight-line analysis applies there), not with
       floating-point loop registers (the merge point would write them back every time). */
    for (i = 0; i < n; i++) { inner[i] = -1; inner_target[i] = 0; inner_fl[i] = 0; inner_on[i] = 0; inner_ret[i] = 0; }
    for (i = 0; i < n; i++) if (is_ret(&b[i], kinds[i], traced[i], keip)) last_eip = i;
    if (!must_check && !g_no_inner && st_k < 0 && !(g_fp_p | g_fp_t))
        for (i = 0; i < n; i++) {
            uint32_t t;
            int j, k;
            if (kinds[i] != JUMP_COND) continue;
            t = b[i].eip + b[i].len + b[i].imm;
            if (t == keip || (g_sl_wb && i <= g_sl_last)) continue;
            for (j = i + 1; j < n && b[j].eip != t; j++) ;
            if (j < n) {                        /* forward: no calls in between */
                for (k = i + 1; k < j; k++)
                    if ((traced[k] && traced[k] != 1) ||
                        (kinds[k] != CONT && kinds[k] != JUMP_COND && !(kinds[k] == JUMP && traced[k]))) break;
                if (k < j) continue;
            } else {                            /* backward (inner loop): j..i without calls */
                /* only inside a loop at the block start: otherwise j itself is a loop head and, as its own
                   block, gets stack slots, constants and an aligned head */
                if (g_sl_wb || g_no_inner_ret || last_eip < 0) continue;
                for (j = 1; j < i && b[j].eip != t; j++) ;
                if (j >= i || j > last_eip) continue;
                for (k = j; k < i; k++)
                    if ((traced[k] && traced[k] != 1) ||
                        (kinds[k] != CONT && kinds[k] != JUMP_COND && !(kinds[k] == JUMP && traced[k]))) break;
                if (k < i) continue;
                inner_ret[j] = 1;
            }
            inner[i] = j;
            inner_target[j] = 1;
            has_inner = 1;
        }
    if (has_inner) {                        /* flags: an inner jump needs what is live at the target (on both paths) */
        live = end_live;
        for (i = n - 1; i >= 0; i--) {
            if (kinds[i] == JUMP_COND) {
                target_needs[i] = needed(b[i].eip + b[i].len + b[i].imm);
                if (inner[i] >= 0) target_needs[i] |= inner[i] == i + 1 ? live : afterwards[inner[i] - 1];
                live |= target_needs[i];
            }
            afterwards[i] = live;
            live = (uint8_t)((live & ~writes[i]) | reads[i]);
        }
        for (i = 0; i < n; i++) if (inner[i] >= 0) inner_fl[inner[i]] |= target_needs[i] | afterwards[inner[i] - 1];
    }
    /* constants: also across calls (const_predict counts eax, ecx, edx as changed by them); prefetch requires that
       the block knows its registers itself: not across calls. ROSETTA_CONST_CALLS=0: not across calls either */
    if (g_const_calls < 0) g_const_calls = getenv("ROSETTA_CONST_CALLS") ? atoi(getenv("ROSETTA_CONST_CALLS")) : 1;
    if (!must_check && !g_no_const && g_cpu && !no_spec(eip) && (g_const_calls || (!ncont && !nstub)) && !var &&
        const_allowed(eip))
        watchdog = const_predict(b, interp, inner_target, n, cval);
    else memset(cval, 0, sizeof cval);
    g_vl_reg = 0;
    if (!must_check && !has_inner && !ncont && !nstub) vl_n = prefetch_choose(b, kinds, traced, interp, cval, n, keip, stream, stream_i, pre_d);
    else memset(stream, 0, sizeof stream);
    memset(pre_on, 0, sizeof pre_on);
    if (!must_check && !g_no_fpahead) npre = fp_hoist(b, kinds, traced, interp, cval, stream, inner_target, n, pre_pos);
    if (vl_n) vl_cont_i = stream_i[vl_n - 1] + 1;
    g_vl_nnew = 0;
    g_vl_overflow = 0;
    g_vl_nloaded = 0;
    if (g_vt_on) { uint64_t t = mach_absolute_time(); g_time_phase[1] += t - g_time_mark; g_time_mark = t; }

    p = g_free;
    start = p;
    for (i = 0; i < 8; i++)                 /* guard: check that the predicted registers are as at translation time */
        if (watchdog >> i & 1) {
            load_imm(0, g_cpu->r[i]);
            e(CMPLW(1, R(i), 0));
            watchdog_bc[nw++] = p;
            e(BC_(BO_FALSE, 4 + CR_EQ, 0));
        }
    if (nw) g_st_watchdog++;
    if (must_check) {                          /* check that the bytes of the block are unchanged */
        uint32_t k, *miss[16];
        int nf = 0;
        load_imm(12, eip);
        for (k = 0; k < a - eip && nf < 16; k++) {
            e(LBZ(3, k, 12));
            e(CMPLWI(0, 3, ld8(eip + k)));
            miss[nf++] = p;
            e(BC_(BO_FALSE, CR_EQ, 0));
        }
        {
            uint32_t *cont = p;
            int j;
            e(B_(0));
            for (j = 0; j < nf; j++) *miss[j] = BC_(BO_FALSE, CR_EQ, (uint32_t)(uintptr_t)p - (uint32_t)(uintptr_t)miss[j]);
            exit_kind(eip, KIND_INVALID);
            *cont = B_((uint32_t)(uintptr_t)p - (uint32_t)(uintptr_t)cont);
        }
    }
    fs.kind = -1; fs.sz = 4; fs.stored = 1;
    memset(g_xf, 0, sizeof g_xf);
    g_xm_ok = 0;
    x7_empty();
    g_ahead_n = g_ahead_i = 0;
    g_sl_nslow = 0;
    if (g_sl_n) {                           /* preload stack slots; the backward jump into the block goes behind this */
        slots_load();
        if (g_sl_wb) nrf = ret_checkcode(ret_miss);    /* check that the other accesses run away from the stack */
        if (g_sl_espfree) {                 /* check that stack write accesses (arguments, dormant calls) lie below the slots */
            int32_t lo = g_sl_d[0];
            for (i = 1; i < g_sl_n; i++) if (g_sl_d[i] < lo) lo = g_sl_d[i];
            addi_after(11, R(ESP), g_sl_smax);
            addi_after(12, R(g_sl_base), lo);
            e(CMPLW(1, 11, 12));
            ret_miss[nrf++] = p;
            e(BC_(BO_TRUE, 4 + CR_GT, 0));
        }
        if (g_sl_verify) addi_after(RRANGE, R(g_sl_base), g_sl_lo);
        g_sl_active = 1;
    }
    if (p == start && g_k_n && !(g_fp_p | g_fp_t) && !vl_n && g_align && last_eip >= 0) {
        /* prologue made only of constants: its length is fixed, so padding goes before it, so the head is aligned
           and an entry from outside runs through no nops */
        int w = 0;
        for (i = 0; i < g_k_n; i++)
            w += ((int32_t)g_k_val[i] >= -32768 && (int32_t)g_k_val[i] < 32768) ? 1 : 1 + ((g_k_val[i] & 0xffff) != 0);
        while (((uintptr_t)(p + w)) & (g_align - 1)) e(0x60000000);
        start = p;
    }
    for (i = 0; i < g_k_n; i++) load_imm(g_k_reg[i], g_k_val[i]);
    for (i = 0; i < 8; i++)                 /* preload the loop's floating-point registers (they count as dirty) */
        if (g_fp_p & (1 << i)) {
            xf_load(i, g_fp_kind[i]);
            g_xf[i].dirty = 1;
            if (g_fp_ph & (1 << i)) { xfh_load_code(i, XFH(i)); g_xf[i].h = 1; g_xf[i].hs = 1; }
        }
    if (g_fp_p | g_fp_t) g_st_fp_loops++;
    if (vl_n) vl_prefetch_code(b, stream_i, pre_d, vl_n);   /* arrays: the first iterations into the ring */
    {                                       /* loop within the block: align the head to a cache line (32 bytes) */
        int j, loop = (end_kind == CONT && a == keip);
        for (j = 0; j < n && !loop; j++)
            if ((kinds[j] == JUMP || kinds[j] == JUMP_COND) && b[j].eip + b[j].len + b[j].imm == keip) loop = 1;
        is_loop = loop;
        if (loop && g_align) {
            int empty = p == start;         /* no prologue: padding before the block start, entry without nops */
            while ((uintptr_t)p & (g_align - 1)) e(0x60000000);   /* nop */
            if (empty) start = p;
        }
    }
    head = p;
    g_xm_ok = 0;                            /* backward jump stores xmm in host form */
    fw_flush();                            /* nothing is forwarded at the head (not at the backward jump either) */
    sw_head(28 + g_sl_n + g_k_n + (g_vl_reg ? 1 : 0));    /* free r28..r31 for stack copies */
    g_sl_wb_now = g_sl_wb;                /* loop part: slots only in the register */
    memcpy(head_xf, g_xf, sizeof g_xf);     /* state at the head (for the exit used to halt) */
    head_wb = g_sl_wb;
    head_wmask = g_sl_wmask;

    /* dead stores (sw_killer): the lookahead needs the kinds of the block's instructions; not with loop slots,
       hoisted floating-point loads or the prefetch ring (they read memory ahead of its instruction) */
    g_sw_kinds = kinds; g_sw_interp = interp; g_sw_traced = traced; g_sw_inner = inner_target; g_sw_after = afterwards;
    g_sw_dead_ok = !g_no_dead && !g_no_sw && !g_no_sw2 && !g_sl_n && !npre && !g_vl_reg;
    for (i = 0; i < n; i++) {
        const Insn *x = &b[i];
        uint32_t following = x->eip + x->len, target = following + x->imm;
        int k;
        g_sw_stk = st_k;
        if (g_sl_wb_now && i == g_sl_last + 1) {   /* after the last backward jump: bring memory up to date */
            sl_back_code(g_sl_wmask);
            g_sl_wb_now = 0;
        }
        if (g_sl_np && i < sl_end) {       /* inlined call: slots are dormant; continue afterwards (reload if it may
                                               have written other memory; a C call may have destroyed r2) */
            int q;
            for (q = 0; q < g_sl_np; q++) {
                if (i == g_sl_pv[q]) g_sl_active = 0;
                if (i == g_sl_pb[q] + 1) {
                    g_sl_active = 1;
                    if (g_sl_pl[q]) slots_load();
                    range_set();
                }
            }
        }
        if (i == sl_end) g_sl_active = 0;
        g_sl_free_now = g_sl_espfree && g_sl_active && g_sl_free[i];
        if (inner_target[i]) {                /* merge point: keep only what holds on all paths */
            int q;
            g_sw_i = i;
            sw_flush_all();                     /* (never needed: sw_killer stops at merge points) */
            for (q = 0; q < i; q++) if (inner_on[q] && inner[q] == i) sw_status_hit(&g_inner_sw[q]);
            if (inner_ret[i]) {           /* later sources unknown: no entries, esp/ebp check them themselves */
                sw_all_drop();
                inner_esp_ok[i] = g_sw_esp_ok; inner_esp[i] = g_sw_esp;
                inner_ebp_ok[i] = g_sw_ebp_ok; inner_ebp[i] = g_sw_ebp;
            }
            fw_flush();
            xf_all_done();
            xf_all_forget();            /* floating point and x87 are in memory at the target */
            x7_all_back();
            if (inner_fl[i]) flags_put();
            fs.kind = -1; fs.stored = 1;   /* flags (if they are live) in memory */
            inner_code[i] = p;
        }
        g_pre_z = 0;
        if (npre) {                         /* issue hoisted floating-point loads here (not into a mov */
            int q;                          /* that takes effect only with this instruction) */
            for (q = i; q < n; q++)
                if (pre_pos[q] >= 0 && pre_pos[q] <= i && !pre_on[q] && (i != g_mov_for || q == i)) {
                    pre_slot[q] = pre_next++ & 31;
                    fp_pre_kick(&b[q], pre_slot[q]);
                    pre_on[q] = 1;
                }
            if (pre_on[i]) { g_pre_z = (int)OFF(fp_pre) + 16 * pre_slot[i]; int gv = fp_load_size(x); g_pre_kind = gv == 16 ? 'P' : gv == 8 ? 'D' : 'F'; }
        }
        g_poison_eip = x->eip;
        g_kaddr = cval[i];
        g_vl_cur = stream[i];
        if (i == vl_cont_i) vl_cont();  /* the last hoisted access of this iteration is over */
        if (i != g_mov_for) g_mov_target = -1;
        if (i == st_k) {                    /* switch: save the index; mov and add are dropped (target from the block's own table) */
            e(MR(6, R(st_x)));
            if (!(g_careful & 4)) continue;
        }
        if (i == st_k + 1 && st_k >= 0 && !(g_careful & 4)) continue;
        fw_pre_insn(x, interp[i], traced[i]);   /* store forwarding: what x writes invalidates entries */
        g_sw_blk = b; g_sw_i = i; g_sw_n = n;
        sw_pre_insn(x, interp[i], traced[i]);   /* stack forwarding: esp/ebp and remembered slots */
        {                                   /* mov r,s before a computation on r: the computation reads s (three operands) */
            int mz, mq;
            if (!g_no_fusion && !interp[i] && kinds[i] == CONT && i + 1 < n && !interp[i + 1] && !inner_target[i + 1] &&
                kinds[i + 1] == CONT && mov_together(x, &b[i + 1], &mz, &mq)) {
                g_mov_target = mz; g_mov_src = mq; g_mov_for = i + 1;
                g_st_mov++;
                continue;
            }
        }
        /* flag-setting instruction whose flags only the next instruction reads: both as one compare */
        if (interp[i]) { gen_interpret(x, reads[i], writes[i]); g_st_inline++; continue; }
        /* floating-point compare with its readers: fcmpu */
        if (!g_no_fusion && fpf_allowed(x->eip) && kinds[i] == CONT && (is_fp_cmp(x) || is_x87_cmp(x)) && i + 1 < n) {
            int j = i + 1, ks[8], nk = 0, ok = 0;
            while (j < n && nk < 8 && !interp[j] && !inner_target[j] && (k = consumer(&b[j])) >= 0 &&
                   !(kinds[j] == JUMP_COND && target_needs[j])) {
                ks[nk++] = k;
                if (!afterwards[j]) { ok = 1; break; }
                j++;
            }
            if (ok && fp_cmp_fusion(x, ks, nk, g_ahead)) {
                if (g_poison) {                   /* poison the flags in memory: whoever reads them later is reported */
                    e(LI(0, 99)); e(STW(0, OFF(cc_op), RCPU));
                    load_imm(0, x->eip); e(STW(0, OFF(cc_res), RCPU));
                }
                g_ahead_n = nk; g_ahead_i = 0;
                g_ahead_data = 1;          /* floating-point compare: rather random */
                fs.kind = -2;
                g_st_fusion++;
                continue;
            }
        }
        /* several readers in a row (cmp; jl; setg): one compare for all */
        if (!g_no_fusion && i + 2 < n && kinds[i] == CONT) {
            int j = i + 1, ks[8], nk = 0, ok = 0;
            while (j < n && nk < 8 && !interp[j] && !inner_target[j] && (k = consumer(&b[j])) >= 0 &&
                   !(cond_mask(k) & ~writes[i]) && !(kinds[j] == JUMP_COND && target_needs[j])) {
                ks[nk++] = k;
                if (!afterwards[j]) { ok = 1; break; }
                j++;
            }
            if (ok && nk >= 2 && chains_fusion(x, ks, nk, g_ahead)) {
                g_ahead_n = nk; g_ahead_i = 0;
                g_ahead_data = x->mem;     /* compare with memory data: condition rather random */
                fs.kind = -2;
                g_st_fusion++;
                continue;
            }
        }
        if (!g_no_fusion && i + 1 < n && kinds[i] == CONT && !interp[i + 1] && !inner_target[i + 1] &&
            (k = consumer(&b[i + 1])) >= 0 &&
            afterwards[i + 1] == 0 && !(cond_mask(k) & ~writes[i]) && fusion(x, k, &g_ahead[0])) {
            g_ahead_n = 1; g_ahead_i = 0;
            g_ahead_data = x->mem;
            fs.kind = -2;
            g_st_fusion++;
            continue;
        }
        /* The only reader comes a few instructions later, with nothing in between that reads or writes flags (gcc
           schedules a load or store between cmp and jcc: cmpl $0x21,%eax; movw %di,(%ecx,%eax,2); jg). Merged all the
           same, the condition moved to cr2 (mcrf): no instruction of the generator uses cr2 and C code keeps it.
           Otherwise the flags went to RE/RA/RB (three mr) and the reader compared again. */
        if (!g_no_fusion && !g_no_far && kinds[i] == CONT && i + 2 < n) {
            int j = i + 1, cr0_kept = !g_vl_reg;
            while (j < n && j <= i + 8 && kinds[j] == CONT && !interp[j] && !traced[j] && !inner_target[j] &&
                   !reads[j] && !writes[j] && j != st_k && j != st_k + 1) {
                if (!cr0_safe(&b[j])) cr0_kept = 0;
                j++;
            }
            if (j > i + 1 && j < n && !interp[j] && !inner_target[j] && (k = consumer(&b[j])) >= 0 &&
                afterwards[j] == 0 && !(cond_mask(k) & ~writes[i]) && !(kinds[j] == JUMP_COND && target_needs[j]) &&
                fusion(x, k, &g_ahead[0])) {
                /* moves, loads and stores in between leave CR alone: the condition stays in cr0; otherwise it moves
                   to cr2 (mcrf costs the G4 more than it looks: with it in every case perl took 1.05 s against 1.00) */
                if (g_ahead[0].bit >= 0 && (!cr0_kept || g_ahead[0].bit >= 4)) {
                    e(MCRF(2, g_ahead[0].bit / 4));
                    g_ahead[0].bit = 8 + g_ahead[0].bit % 4;
                }
                g_ahead_n = 1; g_ahead_i = 0;
                g_ahead_data = x->mem;
                fs.kind = -2;
                g_st_fusion_far++;
                continue;
            }
        }
        switch (kinds[i]) {
        case CONT:
            g_afterwards_now = afterwards[i];
            gen(x, afterwards[i] != 0, (afterwards[i] & FCF) != 0);
            break;
        case JUMP_COND: {
            Cond c = cond_fetch(x->op & 15);
            Page *s;
            if (c.bit < 0 && !c.truth) break;            /* never jumps */
            if (c.bit < 0) { e(CMPW(7, 0, 0)); c = cond_mk(4 * 7 + CR_EQ, 1); }  /* cr7: CR0/cr1 may carry the chain */
            xf_all_done();               /* before the bc: the exit stub reads the FPRs (lfd leaves CR unchanged) */
            s = &pages[ns++];
            s->bc = p;
            s->target = target;
            s->head = (g_sl_active || !g_sl_n) ? head : start;
            s->kind = fs.kind;
            s->sz = fs.sz;
            s->put = target_needs[i] && fs.kind >= 0 && !fs.stored;
            s->indirect = 0;
            s->target2 = 0;
            s->sl_ret = g_sl_wb_now ? g_sl_wmask : 0;
            s->inner = inner[i];
            s->sl_load = 0;
            if (inner[i] >= 0 && inner[i] < i) {    /* backward: esp/ebp must match the state at the target */
                int j = inner[i];
                if ((inner_esp_ok[j] && (!g_sw_esp_ok || g_sw_esp != inner_esp[j])) ||
                    (inner_ebp_ok[j] && (!g_sw_ebp_ok || g_sw_ebp != inner_ebp[j]))) s->inner = -1;
                else s->sl_load = g_sl_n && !g_sl_active && j < sl_end;    /* reload slots after their end */
            }
            if (s->inner >= 0) {                /* inner jump: stack knowledge for the merge point, flags if they are live there */
                s->put = inner_fl[inner[i]] && fs.kind >= 0 && !fs.stored;
                sw_status_note(&g_inner_sw[i]);
                inner_on[i] = 1;
            }
            memcpy(s->xf, g_xf, sizeof g_xf);   /* floating-point state for the exit stub */
            s->x7 = g_x7;
            if (g_sl_wb_now && g_sl_budget && target == keip) {
                /* the backward jump counts the budget down (bdnzt/bdnzf); if it falls through although the condition holds,
                   the budget is used up: write back and re-check through the block start */
                Page *t;
                e(BC_(c.truth ? 8 : 0, c.bit, 0));
                t = &pages[ns++];
                *t = *s;
                t->bc = p;
                t->head = start;
                e(BC_(c.truth ? BO_TRUE : BO_FALSE, c.bit, 0));
                g_st_budget++;
            } else e(BC_(c.truth ? BO_TRUE : BO_FALSE, c.bit, 0));
            break;
        }
        case JUMP:
            if (traced[i]) break;
            xf_all_done();
            if (target == keip && (g_sl_active || !g_sl_n)) xf_sync(g_xf); else xf_all_back();
            x7_all_back();
            if (afterwards[i]) flags_put();
            if (target == keip && g_sl_wb_now && g_sl_budget) {   /* bdnz head; used up: through the start */
                Page *t;
                e(BC_(16, 0, (uint32_t)(uintptr_t)head - (uint32_t)(uintptr_t)p));
                t = &pages[ns++];
                t->bc = p;
                t->target = keip;
                t->head = start;
                t->kind = fs.kind;
                t->sz = fs.sz;
                t->put = 0;
                t->indirect = 0;
                t->target2 = 0;
                t->inner = -1;
                t->sl_ret = g_sl_wmask;
                memcpy(t->xf, g_xf, sizeof g_xf);
                t->x7 = g_x7;
                e(BC_(20, 0, 0));
                g_st_budget++;
            } else if (target == keip) b_to((g_sl_active || !g_sl_n) ? head : start); else stubby(target, 0);
            break;
        case CALL: {                         /* call: record the return, bl into the target (link stack of the G4) */
            uint32_t *bl;
            if (traced[i] == 5) {         /* math function through an import stub: PowerPC function directly, result to st(0) */
                void *native;
                char kind = thunk_kind(stub_fn[i], &native), ga = (kind == '1' || kind == '2') ? 'D' : 'F';
                int two = kind == '2' || kind == 'F', q, f;
                Insn t;
                /* arguments start at esp (no return address in between): from forwarding or via AltiVec */
                memset(&t, 0, sizeof t);
                t.mem = 1; t.base = ESP; t.index = -1; t.disp = 0;
                if ((q = fw_find(&t, ga == 'D' ? 8 : 4)) >= 0) e(FMR(1, q));
                else { int z = vz_new(); vswap_put(R(ESP), ga, z); e(ga == 'D' ? LFD(1, z, RCPU) : LFS(1, z, RCPU)); }
                if (two) {
                    t.disp = ga == 'D' ? 8 : 4;
                    if ((q = fw_find(&t, ga == 'D' ? 8 : 4)) >= 0) e(FMR(2, q));
                    else {
                        int z = vz_new();
                        e(ADDI(12, R(ESP), (int)t.disp));
                        vswap_put(12, ga, z);
                        e(ga == 'D' ? LFD(2, z, RCPU) : LFS(2, z, RCPU));
                    }
                }
                g_c_pure = 1;               /* libm touches no guest memory: host copies stay */
                c_call(native);
                g_c_pure = 0;
                if (g_x7.d < 0 || g_x7.d >= 8) x7_all_back();
                f = x7_push();              /* result in the block's x87 stack (register, not Cpu.st) */
                e(FMR(f, 1));
                g_st_math_direct++;
                break;
            }
            if (traced[i] == 2) {         /* inlined: only the return address onto the Intel stack */
                load_imm(3, following);
                push_reg(3);
                if (g_sw_aesp_ok) sw_const(g_sw_aesp - 4, following);
                g_st_embed++;
                break;
            }
            if (traced[i] == 7) {         /* direct string stub: like its block (esp to the return address,
                                               C wrapper, eax back), but without block switch, shadow stack and ret */
                load_imm(3, following);
                push_reg(3);
                e(STW(R(ESP), OFF(r[ESP]), RCPU));
                e(MR(3, RCPU));
                c_call((void *)stub_fn[i]);
                e(LWZ(R(EAX), OFF(r[EAX]), RCPU));
                e(ADDI(R(ESP), R(ESP), 4));
                fs.kind = -1; fs.stored = 1;
                g_st_stub_direct++;
                break;
            }
            if (traced[i] == 6) {         /* call with continuation: bl to the exit stub (at the block end), execution continues here afterwards */
                xf_all_forget();
                x7_all_back();
                load_imm(3, following);
                push_reg(3);
                if (reads[i]) flags_put();
                call_bl[ncall] = shadow_call(3);
                call_target[ncall++] = target;
                after_call();
                break;
            }
            xf_all_back();
            x7_all_back();
            load_imm(3, following);
            push_reg(3);
            if (afterwards[i]) flags_put();
            bl = shadow_call(3);
            stubby(following, 0);          /* ret returns here with blr */
            *bl = BL_((uint32_t)(uintptr_t)p - (uint32_t)(uintptr_t)bl);
            stubby(target, bl);              /* when chaining, the bl points directly at the target */
            break;
        }
        case RET:
            if (traced[i] == 3) {         /* ret of an inlined function: check the address, continue in the block */
                Page *s;
                int k = g_sw_aesp_ok ? sw_find(g_sw_aesp) : -1;
                if (k >= 0 && g_sw[k].kind == 'k' && g_sw[k].val == expected[i]) {
                    /* return slot provably holds the expected address: neither load nor check */
                    e(ADDI(R(ESP), R(ESP), x->op == 0xc2 ? 4 + (int)(x->imm & 0xffff) : 4));
                    g_st_sw_ret++;
                    break;
                }
                e(LWBRX(3, 0, R(ESP)));
                e(ADDI(R(ESP), R(ESP), x->op == 0xc2 ? 4 + (int)(x->imm & 0xffff) : 4));
                load_imm(4, expected[i]);
                e(CMPLW(7, 3, 4));
                xf_all_done();           /* before the bc: the exit reads the FPRs */
                s = &pages[ns++];
                s->bc = p;
                s->target = 0;
                s->head = 0;
                s->kind = fs.kind;
                s->sz = fs.sz;
                s->put = (g_careful & 2) && fs.kind >= 0 && !fs.stored;
                s->indirect = 1;            /* target is in r3 (unexpected return address) */
                s->target2 = 0;
                s->sl_ret = 0;
                s->inner = -1;
                memcpy(s->xf, g_xf, sizeof g_xf);
                s->x7 = g_x7;
                e(BC_(BO_FALSE, 4 * 7 + CR_EQ, 0));
                break;
            }
            xf_all_back();
            x7_all_back();
            e(LWBRX(3, 0, R(ESP)));
            e(ADDI(R(ESP), R(ESP), x->op == 0xc2 ? 4 + (int)(x->imm & 0xffff) : 4));
            if (afterwards[i]) flags_put();
            shadow_back();
            indirect();                     /* shadow stack does not match: through the table */
            break;
        case JUMP_IND: case CALL_IND: {
            int v;
            if (traced[i] == 8) {         /* variant: the target is fixed (the call site has compared) */
                load_imm(3, following);
                push_reg(3);
                if (g_sw_aesp_ok) sw_const(g_sw_aesp - 4, following);
                break;
            }
            if (traced[i] == 4) {         /* predicted call: check the target, then continue inlined */
                Page *s;
                v = rm_val(x, 4, 3);
                if (v != 3) e(MR(3, v));
                load_imm(4, expected[i]);
                e(CMPLW(7, 3, 4));
                xf_all_done();
                s = &pages[ns++];
                s->bc = p;
                s->target = following;        /* return address for the call in the exit */
                s->head = 0;
                s->kind = fs.kind;
                s->sz = fs.sz;
                s->put = (g_careful & 1) && fs.kind >= 0 && !fs.stored;
                s->indirect = 2;
                s->target2 = 0;
                s->before = expected[i];    /* predicted target (the other known targets go to variants) */
                s->loc = x->eip;
                s->sl_ret = 0;
                s->inner = -1;
                memcpy(s->xf, g_xf, sizeof g_xf);
                s->x7 = g_x7;
                e(BC_(BO_FALSE, 4 * 7 + CR_EQ, 0));
                load_imm(3, following);
                push_reg(3);
                if (g_sw_aesp_ok) sw_const(g_sw_aesp - 4, following);
                g_st_before++;
                break;
            }
            if (traced[i] == 6) {         /* call through a pointer with continuation: inline cache at the block end */
                xf_all_forget();
                x7_all_back();
                v = rm_val(x, 4, 3);
                if (v != 3) e(MR(3, v));
                if (reads[i]) flags_put();
                load_imm(4, following);
                push_reg(4);
                call_bl[ncall] = shadow_call(4);
                call_target[ncall++] = 0;
                after_call();
                break;
            }
            xf_all_back();
            x7_all_back();
            if (i == st_k + 2 && st_k >= 0 && kinds[i] == JUMP_IND) {   /* switch: own table with host addresses */
                if (afterwards[i]) flags_put();
                jumptab_code(x->rm, st_t, st_b, (int32_t)b[st_k].disp);
                break;
            }
            v = rm_val(x, 4, 3);
            if (v != 3) e(MR(3, v));
            if (afterwards[i]) flags_put();
            if (kinds[i] == CALL_IND) {
                uint32_t *bl;
                load_imm(4, following);
                push_reg(4);
                bl = shadow_call(4);
                stubby(following, 0);
                *bl = BL_((uint32_t)(uintptr_t)p - (uint32_t)(uintptr_t)bl);
            }
            indirect();
            break;
        }
        }
    }
    sw_flush_all();                         /* (never needed: the overwriting instruction is in the block) */
    g_sw_skip = g_sw_skipped = 0;
    g_sw_dead_ok = 0;
    g_sw_kinds = 0;
    g_kaddr = 0;
    g_vl_cur = 0;
    g_pre_z = 0;
    g_sl_free_now = 0;
    g_mov_target = -1;
    g_mov_for = -1;
    if (g_sl_wb_now) {                    /* last backward jump was the last instruction: falling through writes back */
        sl_back_code(g_sl_wmask);
        g_sl_wb_now = 0;
    }
    if (end_kind == CONT) {
        xf_all_done();
        if (a == keip && !can_not && (g_sl_active || !g_sl_n)) xf_sync(g_xf); else xf_all_back();
        x7_all_back();
        if (end_live) flags_put();
        if (can_not) exit_kind(a, KIND_INTERPRET);
        else if (a == keip) b_to((g_sl_active || !g_sl_n) ? head : start);
        else stubby(a, 0);
    }
    g_sl_active = 0;
    g_xm_ok = 0;                            /* exit stubs: tag bytes depend on the path, so always check */
    {                                       /* main path finished: pending loads do not apply to exit stubs */
        int k;
        for (k = 0; k < 8; k++) { g_xf[k].pending = 0; g_xf[k].hhome = 0; }
    }
    if (g_vt_on) { uint64_t t = mach_absolute_time(); g_time_phase[2] += t - g_time_mark; g_time_mark = t; }
    /* calls with continuation: their bl leads here (exit stub to the target or inline cache of the pointer in r3) */
    for (i = 0; i < ncall; i++) {
        *call_bl[i] = BL_((uint32_t)(uintptr_t)p - (uint32_t)(uintptr_t)call_bl[i]);
        if (call_target[i]) stubby(call_target[i], call_bl[i]); else indirect();
    }
    /* memory access hit the stack slots: reload and jump back */
    for (i = 0; i < g_sl_nslow; i++) {
        bc_set(g_sl_slow[i], p);
        slots_load();
        b_to(g_sl_slow[i] + 1);
    }
    /* side exits: backward jump to the block start directly, otherwise exit stub (with flags if the target reads them) */
    for (i = 0; i < ns; i++) {
        Page *s = &pages[i];
        uint32_t *start = p;
        if (s->indirect) {                  /* unexpected return address / call target (in r3): through the dispatcher */
            bc_set(s->bc, p);
            if (s->target2) load_imm(3, s->target2);    /* direct math: continue at the import stub itself */
            e(STW(3, OFF(eip), RCPU));
            xf_state_back(s->xf);
            x7_back_code(&s->x7);
            if (s->put) { fs.kind = s->kind; fs.sz = s->sz; fs.stored = 0; flags_put(); }
            if (s->indirect == 2) {         /* known other targets: their variant; otherwise execute the call itself and
                                               learn the target (block rebuilt, from the fifth target on without prediction) */
                TargetList *zl = g_no_variants ? 0 : targets_find(s->loc);
                uint32_t *beq[VAR_TARGETS], vz[VAR_TARGETS];
                int nv = 0, k;
                if (zl) {
                    e(LWZ(3, OFF(eip), RCPU));
                    for (k = 0; k < zl->n; k++)
                        if (zl->target[k] != s->before) {
                            load_imm(4, zl->target[k]);
                            e(CMPLW(7, 3, 4));
                            beq[nv] = p;
                            vz[nv++] = zl->target[k];
                            e(BC_(BO_TRUE, 4 * 7 + CR_EQ, 0));
                        }
                }
                load_imm(4, s->target);
                push_reg(4);
                load_imm(5, eip);
                e(STW(5, OFF(spec_miss), RCPU));
                load_imm(5, s->loc);
                e(STW(5, OFF(spec_loc), RCPU));
                e(LWZ(3, OFF(eip), RCPU));
                e(LI(4, KIND_NORMAL));
                b_to(exit_near(p));
                for (k = 0; k < nv; k++) {  /* exit stub to the variant: target in Cpu.var_target, eip = call site */
                    bc_set(beq[k], p);
                    e(LIS(3, vz[k] >> 16)); e(ORI(3, 3, vz[k] & 0xffff));
                    e(STW(3, OFF(var_target), RCPU));
                    stubby(s->loc, beq[k]);
                }
                continue;
            }
            e(LWZ(3, OFF(eip), RCPU));
            e(LI(4, KIND_NORMAL));
            b_to(exit_near(p));
            continue;
        }
        if (s->inner >= 0) {                /* inner jump: floating point and flags as at the merge point, then jump in */
            xf_state_back(s->xf);
            x7_back_code(&s->x7);
            if (s->put) { fs.kind = s->kind; fs.sz = s->sz; fs.stored = 0; flags_put(); }
            if (s->sl_load) { slots_load(); if (g_sl_verify) addi_after(RRANGE, R(g_sl_base), g_sl_lo); }
            if (p != start) { bc_set(s->bc, start); b_to(inner_code[s->inner]); }
            else bc_set(s->bc, inner_code[s->inner]);
            g_st_inner++;
            continue;
        }
        /* stack slots (deferred) to memory, except on the backward jump to the loop head */
        if (s->sl_ret && !(s->target == keip && s->head == head)) sl_back_code(s->sl_ret);
        /* floating point: sync to the loop head, otherwise write back everything dirty; then flags */
        if (s->target == keip && s->head == head) xf_sync(s->xf); else xf_state_back(s->xf);
        x7_back_code(&s->x7);            /* x87: at the target (also at the loop head) everything is in Cpu.st */
        {   /* Jump threading: the target starts with a conditional jump and the flags are still in registers: decide here
               and go straight to its target (the target block would otherwise have to evaluate them from memory) */
            Insn t;
            uint8_t tl, ts;
            int ta;
            if (s->target != keip && s->kind >= 0 && !g_no_chain && s->target - COMMPAGE_BASE >= COMMPAGE_SIZE &&
                page_readonly(s->target) && page_readonly(s->target + 15) && decode(s->target, &t) &&
                classify(&t, &tl, &ts, &ta) && ta == JUMP_COND) {
                uint32_t za = t.eip + t.len + t.imm, zb = t.eip + t.len, *bc2;
                int nstored;
                Cond c;
                bc_set(s->bc, start);
                fs.kind = s->kind; fs.sz = s->sz; fs.stored = !s->put;
                c = cond(t.op & 15);
                if (c.bit < 0) {
                    uint32_t z = c.truth ? za : zb;
                    if (needed(z)) flags_put();
                    stubby(z, 0);
                } else {
                    bc2 = p;
                    e(BC_(c.truth ? BO_TRUE : BO_FALSE, c.bit, 0));
                    nstored = fs.stored;
                    if (needed(zb)) flags_put();
                    stubby(zb, 0);
                    fs.stored = nstored;
                    bc_set(bc2, p);
                    if (needed(za)) { flags_put(); stubby(za, 0); } else stubby(za, bc2);
                }
                g_st_chain++;
                continue;
            }
        }
        if (s->put) { fs.kind = s->kind; fs.sz = s->sz; fs.stored = 0; flags_put(); }
        if (p != start) {
            bc_set(s->bc, start);
            if (s->target == keip) b_to(s->head); else stubby(s->target, 0);
        } else if (s->target == keip) bc_set(s->bc, s->head);
        else { bc_set(s->bc, p); stubby(s->target, s->bc); }
    }
    if (is_loop && head != start && !must_check) {
        /* Exit used to halt at the loop head (jit_interrupt redirects the head here): at the head x87 and flags are
           already in Cpu, preloaded floating-point values and deferred stack slots still in registers */
        uint32_t *exit_ptr = p;
        if (head_wb) sl_back_code(head_wmask);
        xf_state_back(head_xf);
        exit_kind(keip, KIND_NORMAL);
        head_note(head, exit_ptr);
    }
    if (nw) {                               /* guard failed: retranslate without prediction */
        for (i = 0; i < nw; i++) bc_set(watchdog_bc[i], p);
        exit_kind(eip, KIND_WATCHDOG);
    }
    if (nrf) {                              /* an access lies on the wrong side of the slots: retranslate without deferral */
        for (i = 0; i < nrf; i++) bc_set(ret_miss[i], p);
        exit_kind(eip, KIND_RET);
    }
    if (vl_n) {
        /* every stream must have gone through vl_load exactly once, otherwise a ring slot would stay stale */
        if (g_vl_nloaded != vl_n || g_vl_overflow) {
            if (g_vt_on) fprintf(stderr, "rosetta: prefetch at %08x discarded (%d of %d streams)\n", eip, g_vl_nloaded, vl_n);
            g_vl_forbidden = eip;
            g_vl_reg = 0;
            return translate(eip);
        }
        for (i = 0; i < g_vl_nnew; i++) vl_pc_add(g_vl_new[i]);
        g_vl_nring++;
        g_st_prefetch++;
        g_st_vl_streams += (unsigned long)vl_n;
    }
    g_vl_reg = 0;
    g_k_n = 0;
    {                                       /* Debugging: ROSETTA_SHOW=lo-hi writes blocks to /tmp/rosetta_blocks.s */
        static int was_read;
        static uint32_t zlo = 1, zhi = 0;
        if (!was_read) { const char *z = getenv("ROSETTA_SHOW"); was_read = 1; if (z) sscanf(z, "%x-%x", &zlo, &zhi); }
        if (eip >= zlo && eip <= zhi) {
            FILE *zf = fopen("/tmp/rosetta_blocks.s", "a");
            uint32_t *w;
            if (zf) {
                fprintf(zf, "b_%08x:\n", eip);
                fprintf(zf, "; host %p\n", (void *)start);
                for (i = 0; i < n; i++) fprintf(zf, "; %08x\n", b[i].eip);
                for (w = start; w < p; w++) fprintf(zf, "\t.long 0x%08x\n", *w);
                fclose(zf);
            }
        }
    }
    g_free = p;
    sys_icache_invalidate(start, (size_t)((uint8_t *)p - (uint8_t *)start));
    if (!var) map_add(eip, start);     /* variants are in g_var */
    else g_st_variants++;
    {
        uint32_t sp[DEP_SPANS] = { 0 };
        dep_note(sp, eip);
        for (i = 0; i < n; i++) {           /* pages this block comes from (code, predicted constants) */
            page_note(b[i].eip);
            dep_note(sp, b[i].eip);
            if (b[i].len) { page_note(b[i].eip + b[i].len - 1); dep_note(sp, b[i].eip + b[i].len - 1); }
            if (cval[i]) { page_note(cval[i]); page_note(cval[i] + 15); dep_note(sp, cval[i]); dep_note(sp, cval[i] + 15); }
        }
        dep_add(eip, start, sp);
    }
    g_st_blocks++;
    g_st_insns += (unsigned long)n;
    g_st_pages += (unsigned long)ns;
    if (g_vt_on) { uint64_t t = mach_absolute_time(); g_time_phase[3] += t - g_time_mark; g_time_mark = t; }
    return start;
}

/* ---------- Variants of indirect calls ---------- */

static TargetList *targets_find(uint32_t loc)
{
    uint32_t h = (loc * 2654435761u) >> 23, i;
    for (i = 0; i < 512; i++, h = (h + 1) & 511) {
        if (g_targets[h].loc == loc) return &g_targets[h];
        if (!g_targets[h].loc) return 0;
    }
    return 0;
}

/* Target observed at call site loc. 1: the site keeps prediction and variants; 0: too many or
   non-inlinable targets (then call generically) */
static int targets_note(uint32_t loc, uint32_t target)
{
    uint32_t h = (loc * 2654435761u) >> 23, i;
    TargetList *z = 0;
    int k;
    if (target - COMMPAGE_BASE < COMMPAGE_SIZE || !page_readonly(target) || !page_readonly(target + 15) ||
        thunk_find(target) || !embed_worth(target, 24)) return 0;
    for (i = 0; i < 512; i++, h = (h + 1) & 511)
        if (g_targets[h].loc == loc || !g_targets[h].loc) { z = &g_targets[h]; break; }
    if (!z) return 0;
    z->loc = loc;
    for (k = 0; k < z->n; k++) if (z->target[k] == target) return 1;
    if (z->n >= VAR_TARGETS) return 0;
    z->target[z->n++] = target;
    g_st_var_targets++;
    return 1;
}

/* variant (call site loc, target target): exists or translated now; 0: not possible */
static uint32_t *variant(uint32_t loc, uint32_t target)
{
    uint32_t h = ((loc ^ (target * 31u)) * 2654435761u) >> 23, i, *code;
    for (i = 0; i < VAR_N; i++, h = (h + 1) & (VAR_N - 1)) {
        if (!g_var[h].code) break;
        if (g_var[h].loc == loc && g_var[h].target == target) return g_var[h].code;
    }
    if (i == VAR_N) return 0;
    g_var_active = 1; g_var_loc = loc; g_var_target = target;
    code = translate(loc);
    g_var_active = 0;
    if (!code) return 0;
    h = ((loc ^ (target * 31u)) * 2654435761u) >> 23;     /* (translate may have flushed) */
    for (i = 0; i < VAR_N && g_var[h].code; i++, h = (h + 1) & (VAR_N - 1)) ;
    if (i < VAR_N) { g_var[h].loc = loc; g_var[h].target = target; g_var[h].code = code; }
    return code;
}

/* ---------- Dispatcher ---------- */

/* exit stub becomes "b target"; the conditional jump that leads to the exit stub points straight at the target where possible */
static void chain_to(uint32_t *st, uint32_t *target)
{
    uint32_t *bc = (uint32_t *)(uintptr_t)st[3];
    if (b_reaches(st, target)) {
        *st = B_((uint32_t)(uintptr_t)target - (uint32_t)(uintptr_t)st);
        sys_icache_invalidate(st, 4);
    } else {
        far_jump(st, target);               /* the other half: the stub's four words (its last, bc, was read above) */
        g_st_far++;
    }
    if (bc) {
        int32_t d = (int32_t)((uint32_t)(uintptr_t)target - (uint32_t)(uintptr_t)bc);
        if ((*bc >> 26) == 18) {            /* bl of a call: within the half (otherwise it stays on the stub) */
            if (!b_reaches(bc, target)) { g_st_linked++; return; }
            *bc = (*bc & 0xfc000003u) | ((uint32_t)d & 0x03fffffcu);
            sys_icache_invalidate(bc, 4);
            g_st_direct++;
        } else if (d >= -32768 && d < 32768) {
            bc_set(bc, target);
            sys_icache_invalidate(bc, 4);
            g_st_direct++;
        }
    }
    g_st_linked++;
}

static void redirect(uint32_t *prev, uint32_t *fresh)
{
    if (!b_reaches(prev, fresh)) {          /* the other half: through the pool of prev's half */
        uint32_t *s = pool_take(prev);
        if (!s) { g_flush = 3; return; }    /* full: flush at the next round (the old block's checks hold until then) */
        far_jump(s, fresh);
        fresh = s;
        g_st_far++;
    }
    *prev = B_((uint32_t)(uintptr_t)fresh - (uint32_t)(uintptr_t)prev);
    sys_icache_invalidate(prev, 4);
}

/* Remapped ranges (g_flush 2; thread lock held, no thread in translated code): discard the blocks that rely on
   them. Each one's start jumps to an exit stub that chains to the new translation at its first use, so jumps
   already chained to the old block keep working. Map, jump table and variants forget them, the code pages of the
   ranges are free again, and the return rings start anew (their host addresses can point into discarded blocks).
   Too little space for the exit stubs: flush everything */
static void invalidate_ranges(void)
{
    uint32_t p0[INVAL_N], p1[INVAL_N], i, n = 0, loaded = 0, *keep = p, *stubs;
    uint32_t off_loaded = (uint32_t)((uint8_t *)g_free_loaded - (uint8_t *)g_cache);
    int r, wb = g_sl_wb_now;
    if (g_dep_lost) { flush(); return; }
    for (r = 0; r < g_ninval; r++) {
        p0[r] = g_inval[r].a >> 12;
        p1[r] = (uint32_t)(((uint64_t)g_inval[r].a + (g_inval[r].len ? g_inval[r].len : 1) - 1) >> 12);
    }
    for (i = 0; i < g_ndep; i++) {
        BlockDep *d = &g_dep[i];
        if (d->code & 1) continue;
        for (r = 0; r < g_ninval && !dep_hits(d, p0[r], p1[r]); r++) ;
        if (r < g_ninval) { d->code |= 2; n++; }
    }
    {
        unsigned long l = g_st_flushes;
        room(4 * n + 8192);
        if (l != g_st_flushes) return;     /* no room: everything went */
    }
    g_sl_wb_now = 0;
    p = stubs = g_free;
    for (i = 0; i < g_ndep; i++) {
        BlockDep *d = &g_dep[i];
        uint32_t *code;
        if (!(d->code & 2)) continue;
        d->code = (d->code & ~3u) | 1;
        code = g_cache + (d->code >> 2);
        if (d->code < off_loaded) loaded++;
        if (g_cache_show > 1) fprintf(stderr, "rosetta: discarded %08x%s\n", d->eip, d->code < off_loaded ? " (translation cache)" : "");
        if (map_find(d->eip) == code) map_remove(d->eip);
        if (!b_reaches(code, p + 4)) {         /* a block in the other half: its exit stub in that half's pool */
            uint32_t *s = pool_take(code), *q = p;
            if (!s) {
                p = keep;
                g_sl_wb_now = wb;
                flush_full();
                return;
            }
            *code = B_((uint32_t)(uintptr_t)s - (uint32_t)(uintptr_t)code);
            sys_icache_invalidate(code, 4);
            p = s;
            stubby(d->eip, 0);
            sys_icache_invalidate(s, 16);
            p = q;
            continue;
        }
        *code = B_((uint32_t)(uintptr_t)p - (uint32_t)(uintptr_t)code);
        sys_icache_invalidate(code, 4);
        stubby(d->eip, 0);
    }
    sys_icache_invalidate(stubs, (size_t)((uint8_t *)p - (uint8_t *)stubs));
    g_free = p;
    p = keep;
    g_sl_wb_now = wb;
    if (n) {
        for (i = 0; i < VAR_N; i++) {     /* variants: tombstone (the probe chains stay intact) */
            BlockDep *d;
            if (!g_var[i].code || g_var[i].loc == 0xffffffffu) continue;
            if ((d = dep_find(g_var[i].code)) != 0 && (d->code & 1)) g_var[i].loc = g_var[i].target = 0xffffffffu;
        }
        for (i = 0; i < (uint32_t)g_nfcpu; i++) g_fcpu[i]->shadow = g_fcpu[i]->shadow_base;
    }
    for (r = 0; r < g_ninval; r++) {      /* nothing relies on these pages any more */
        uint32_t pg;
        for (pg = p0[r]; pg <= p1[r]; pg++) {
            g_codepages[pg >> 5] &= ~(1u << (pg & 31));
            g_newpages[pg >> 5] &= ~(1u << (pg & 31));
        }
    }
    g_st_inval_ranges += (unsigned long)g_ninval;
    g_st_inval_blocks += n;
    g_st_inval_loaded += loaded;
    if (g_cache_show) fprintf(stderr, "rosetta: %u blocks discarded (%u from the translation cache) for %d ranges\n", n, loaded, g_ninval);
    g_ninval = 0;
    g_flush = 0;
}

/* frequency of interpreted instructions (ROSETTA_STATS): shows what the translator should still learn */
static unsigned long g_interp_op[0x400];

static void interp_tally(uint32_t eip)
{
    Insn b;
    if (!decode(eip, &b)) return;
    if (b.op >= 0xd8 && b.op <= 0xdf) g_interp_op[0x3c0 + (b.op - 0xd8) * 8 + b.reg]++;     /* x87 per reg */
    else g_interp_op[b.op & 0x3ff]++;
}

/* addresses at which the dispatcher enters (ROSETTA_STATS) */
static struct { uint32_t eip, kind; unsigned long n; } g_vt[1024];

static void vt_tally(uint32_t eip, uint32_t kind)
{
    uint32_t h = (eip * 2654435761u) >> 22, i;
    for (i = 0; i < 1024; i++, h = (h + 1) & 1023)
        if (g_vt[h].eip == eip || !g_vt[h].n) { g_vt[h].eip = eip; g_vt[h].kind = kind; g_vt[h].n++; return; }
}

static int step_counted(Cpu *c)
{
    interp_tally(c->eip);
    return step(c);
}

/* the most frequent interpreted opcodes so far (ROSETTA_STATS, at the first idle moment; leaves the counts) */
static void interp_op_show(void)
{
    unsigned long seen[16];
    int k, i, best, m;
    fprintf(stderr, "rosetta: interpreted so far (opcode count):");
    for (k = 0; k < 16; k++) {
        for (best = -1, i = 0; i < 0x400; i++) {
            for (m = 0; m < k && seen[m] != (unsigned long)i; m++) ;
            if (m == k && g_interp_op[i] && (best < 0 || g_interp_op[i] > g_interp_op[best])) best = i;
        }
        if (best < 0) break;
        seen[k] = (unsigned long)best;
        if (best >= 0x3c0) fprintf(stderr, " %02x/%d:%lu", 0xd8 + ((best - 0x3c0) >> 3), (best - 0x3c0) & 7, g_interp_op[best]);
        else fprintf(stderr, " %s%02x:%lu", best >= 0x300 ? "0f3a." : best >= 0x200 ? "0f38." : best >= 0x100 ? "0f." : "",
                     best & 0xff, g_interp_op[best]);
    }
    fprintf(stderr, "\n");
}

/* ---------- Tiered translation ----------
 * Translation costs about 3 us per instruction, interpretation a fraction of that. Code that runs only a few times
 * (the bulk at startup) is therefore interpreted; a block is translated only when the dispatcher enters it for the
 * g_threshold-th time (ROSETTA_THRESHOLD, 1 = immediately as before). Counting happens only at the dispatcher: jumps
 * between translated blocks are chained and do not pass there. */
/* Counter per address: open hash table that grows fourfold at half occupancy. Never evict: with a fixed
   size (8192, evicting after 8 probes) the counters for many cold addresses kept starting over at 0, blocks
   stayed interpreted forever (ls with 2048 slots: 145 -> 237 ms). Start small: locality at startup */
typedef struct { uint32_t eip, n; } Counter;
static Counter *g_count;
static uint32_t g_count_bits, g_count_used;
static int g_threshold = 10;            /* ROSETTA_THRESHOLD (with translation cache ROSETTA_CACHE_THRESHOLD); 14 Nov: hello best at 12-15, ls 10-12, Foundation 6-10 */
static uint32_t g_counted;             /* already counted (exit from an exit stub): do not count again above */
static unsigned long g_st_cold, g_st_cold_insn;

static void counter_grow(void)
{
    Counter *prev = g_count, *fresh;
    uint32_t oldn = g_count ? 1u << g_count_bits : 0, bits = g_count ? g_count_bits + 2 : 12, i;
    fresh = calloc(1u << bits, sizeof *fresh);
    if (!fresh) return;                   /* stays stale (then fills beyond half, the search still terminates) */
    for (i = 0; i < oldn; i++)
        if (prev[i].eip) {
            uint32_t h = (prev[i].eip * 2654435761u) >> (32 - bits);
            while (fresh[h].eip) h = (h + 1) & ((1u << bits) - 1);
            fresh[h] = prev[i];
        }
    g_count = fresh;
    g_count_bits = bits;
    free(prev);
}

/* Debugging: ROSETTA_ONLY=lo-hi (hex) translates only blocks that start in this range (the rest is interpreted;
   needs ROSETTA_THRESHOLD > 1 and ROSETTA_NO_CACHE) */
static int only_allowed(uint32_t eip)
{
    static int was_read;
    static uint32_t lo = 0, hi = 0xffffffffu;
    if (!was_read) {
        const char *s = getenv("ROSETTA_ONLY");
        was_read = 1;
        if (s) sscanf(s, "%x-%x", &lo, &hi);
    }
    return (eip >= lo && eip <= hi) || commpage_stub(eip) || thunk_find(eip);
}

/* Late threshold: short programs start fastest with threshold 10 (most of their code runs once: hello 30 against 70 ms,
   perl -e 80 against 150 ms with 1), programs with windows with 1 (AppKit's startup runs much code a few times: Calculator's
   first start 4.7 against 6.8 s processor time). So the threshold drops to ROSETTA_THRESHOLD_LATE (1) once the program has
   run ROSETTA_THRESHOLD_MS (300) ms; not with an explicit ROSETTA_THRESHOLD. */
static int g_threshold_late = -1;
static uint64_t g_threshold_at;

static void threshold_late_check(void)
{
    if (g_threshold_late < 0) {
        mach_timebase_info_data_t tb;
        const char *l = getenv("ROSETTA_THRESHOLD_LATE"), *ms = getenv("ROSETTA_THRESHOLD_MS");
        mach_timebase_info(&tb);
        g_threshold_late = getenv("ROSETTA_THRESHOLD") ? g_threshold : l ? atoi(l) : 1;
        if (g_threshold_late < 1) g_threshold_late = 1;
        g_threshold_at = mach_absolute_time() + (uint64_t)(ms ? atoi(ms) : 300) * 1000000ull * tb.denom / tb.numer;
    }
    if (g_threshold > g_threshold_late && mach_absolute_time() > g_threshold_at) g_threshold = g_threshold_late;
}

static int often_enough(uint32_t eip)
{
    uint32_t h, msk;
    if (g_threshold <= 1) return 1;
    threshold_late_check();
    if (!only_allowed(eip)) return 0;
    g_counted = eip;
    if (!g_count || 2 * (g_count_used + 1) > (1u << g_count_bits)) counter_grow();
    if (!g_count) return 1;
    msk = (1u << g_count_bits) - 1;
    for (h = (eip * 2654435761u) >> (32 - g_count_bits); g_count[h].eip; h = (h + 1) & msk)
        if (g_count[h].eip == eip) return ++g_count[h].n >= (uint32_t)g_threshold;
    g_count[h].eip = eip;
    g_count_used++;
    /* never interpret a function with a native version (thunk.c): its state (strtok, malloc) lives in the host, the
       Intel version would work with its own. Look it up once per address, the counter is then already full */
    g_count[h].n = (commpage_stub(eip) || thunk_find(eip)) ? (uint32_t)g_threshold : 1;
    return g_count[h].n >= (uint32_t)g_threshold;
}

/* ---------- Prior knowledge: hot blocks from earlier runs ----------
 * Almost all interpretation at startup is the warm-up of blocks that are translated later anyway (threshold-1
 * runs per block; at Foundation startup as many instructions as are interpreted in total). At the end of a run
 * the list of translated blocks per program goes to ~/Library/Caches/rosetta/hot-...; the next time their
 * counters start out full. A wrong entry costs only one translation, never correctness.
 * ROSETTA_NO_PRIOR=1 turns it off. */
#define HOT_MAX 65536
static char g_hot_path[1024];
static uint32_t *g_hot, g_hot_n;
static unsigned long g_st_prior;

static uint32_t fnv(const char *s, uint32_t h)
{
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static char g_prog_full[1024];
void jit_prog(const char *path)
{
    if (getenv("ROSETTA_STATE")) signal(SIGUSR2, state_signal);
    char full[1024];
    const char *home = getenv("HOME");
    if (!realpath(path, full)) return;
    snprintf(g_prog_full, sizeof g_prog_full, "%s", full);
    if (!home || getenv("ROSETTA_NO_PRIOR")) return;
    /* key: program and build of rosetta (other build, other blocks) */
    snprintf(g_hot_path, sizeof g_hot_path, "%s/Library/Caches/rosetta/hot-%08x-%08x", home,
             fnv(full, 2166136261u), fnv(__DATE__ " " __TIME__, 2166136261u));
}

static void counter_full(uint32_t eip)
{
    uint32_t h, msk;
    if (!eip) return;
    if (!g_count || 2 * (g_count_used + 1) > (1u << g_count_bits)) counter_grow();
    if (!g_count) return;
    msk = (1u << g_count_bits) - 1;
    for (h = (eip * 2654435761u) >> (32 - g_count_bits); g_count[h].eip; h = (h + 1) & msk)
        if (g_count[h].eip == eip) { g_count[h].n = (uint32_t)g_threshold; return; }
    g_count[h].eip = eip;
    g_count[h].n = (uint32_t)g_threshold;
    g_count_used++;
}

static void hot_load(void)
{
    uint32_t head[2], k;
    int fd;
    if (!g_hot_path[0] || g_threshold <= 1 || (fd = open(g_hot_path, O_RDONLY)) < 0) return;
    if (read(fd, head, 8) == 8 && head[0] == 0x48454953u && head[1] <= HOT_MAX && (g_hot = malloc(4 * (size_t)head[1] + 4)) &&
        read(fd, g_hot, 4 * (size_t)head[1]) == (ssize_t)(4 * (size_t)head[1])) {
        g_hot_n = head[1];
        for (k = 0; k < g_hot_n; k++) counter_full(g_hot[k]);
        g_st_prior = g_hot_n;
    } else g_hot_n = 0;
    close(fd);
}

static int u32_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* ---------- Translation cache ----------
 * At the end of a run the translated code is saved with everything it relies on (map, code pages,
 * constant pool, prefetch sites, variants) and read back at the next start of the same program. This works
 * because all addresses are fixed: code cache at CACHE_ADDR, rosetta itself without PIC, guest libraries rebased
 * (intel_rebase.sh), dyld and program fixed. Key: program, build of rosetta, ROSETTA_* environment and addresses of
 * the host library (native math functions). It is loaded only if every guest library of the saved run is unchanged
 * as a file (size, modification time); if dyld maps it at the expected place, nothing is discarded
 * (jit_mapping_expected), any other mapping over translated pages discards the blocks that rely on those pages
 * (invalidate_ranges; a library at another address than in the saved run costs only its own blocks).
 * ROSETTA_NO_CACHE=1 */
#define CACHE_MAGIC 0x564f5238u
#define CACHE_IMAGES 128
typedef struct {
    uint32_t magic, cache, prolog, code, nmap, npages, ncpool, nvlpc, vlring, nvar, nimages, codeoffset, ntab, nhead, ndep;
    uint32_t nmapping, nvpool;
} CacheHead;
typedef struct { uint32_t start, end, size, mtime, ino, type; char path[232]; } CacheImage;
static char g_cache_path[1024];
static uint32_t g_written_live;         /* live blocks in the cache file written by this run (checkpoint) */
static uint32_t g_cache_cap;            /* ROSETTA_CACHE_CAP_MB: code in the file at most (cache_write; the first half) */
static uint32_t g_cap_at;               /* code size at which the dispatcher writes a checkpoint (0: done) */
static unsigned long g_inval_written;   /* g_st_inval_blocks at that write */
static CacheImage g_vimage[CACHE_IMAGES];
static int g_nvimage, g_cache_loaded;
static unsigned long g_st_cache, g_st_expected;
static uint64_t g_cache_time;
static int g_cache_error;
const char *sys_guest_path(const char *p);    /* sys.c: path with Intel redirection */

static uint32_t env_scatter(void)
{
    extern char **environ;
    char **u;
    uint32_t h = (uint32_t)(uintptr_t)&memmove * 3u ^ (uint32_t)(uintptr_t)&sin * 5u ^ (uint32_t)(uintptr_t)&strlen * 7u;
    for (u = environ; *u; u++)              /* order does not matter; profile, log, image list do not change the code */
        if (!strncmp(*u, "ROSETTA_", 8) && strncmp(*u, "ROSETTA_HPROFILE", 16) && strncmp(*u, "ROSETTA_LOG", 11) &&
            strncmp(*u, "ROSETTA_IMAGES", 14) && strncmp(*u, "ROSETTA_CACHE_SHOW", 18) && strncmp(*u, "ROSETTA_CACHE_FILE", 18) &&
            strncmp(*u, "ROSETTA_CACHE_MAX_MB", 20) && strncmp(*u, "ROSETTA_CACHE_CAP_MB", 20))
            h += fnv(*u, 2166136261u);
    return h;
}

/* ~/Library/Caches/rosetta/cache-PROGRAM-BUILD-ENVIRONMENT, or ROSETTA_CACHE_FILE (tests) */
static void cache_path(void)
{
    const char *home = getenv("HOME"), *file = getenv("ROSETTA_CACHE_FILE");
    if (g_cache_path[0] || getenv("ROSETTA_NO_CACHE")) return;
    if (file && *file) { snprintf(g_cache_path, sizeof g_cache_path, "%s", file); return; }
    if (!home || !g_prog_full[0]) return;
    snprintf(g_cache_path, sizeof g_cache_path, "%s/Library/Caches/rosetta/cache-%08x-%08x-%08x", home,
             fnv(g_prog_full, 2166136261u), fnv(__DATE__ " " __TIME__, 2166136261u), env_scatter());
}

/* Files the guest mapped with MAP_FIXED (jit_mapping_expected): which file is where. The install name of an
   image is no use for this (libperl is called /System/Library/Perl/lib/5.8/libperl.dylib, the file loaded is
   .../5.8.8/darwin-thread-multi-2level/CORE/libperl.dylib) */
#define MAPPING_N 4096
typedef struct { uint32_t start, end, ino, size, mtime, off; } Mapping;    /* ino 0: anonymous */
typedef char mapping_six_words[sizeof(Mapping) == 24 ? 1 : -1];
static Mapping g_mapping[MAPPING_N];
static int g_nmapping;
static Mapping g_vmapping[MAPPING_N];  /* those of the run that wrote the translation cache (oldest first) */
static int g_nvmapping;
static int g_fs_fd = -1;                /* fstat of the descriptor checked last (sys.c forgets it on close etc.) */
static struct stat g_fs;

/* Where the run that wrote the translation cache had the library whose architecture starts at file offset off of
   the file open as fd (its first segment mapped from there), 0 if nowhere. Libraries without a fixed address
   (Firefox's own, vmaddr 0) land wherever dyld's vm_allocate(anywhere) puts them, differently from run to run, and
   their translated blocks were discarded each time (mach_msg.c asks here and reserves that place first). */
uint32_t jit_cached_image(int fd, uint32_t off)
{
    struct stat st;
    int i;
    if (!g_cache_loaded || fd < 0 || fstat(fd, &st)) return 0;
    for (i = 0; i < g_nvmapping; i++)
        if (g_vmapping[i].ino == (uint32_t)st.st_ino && g_vmapping[i].size == (uint32_t)st.st_size &&
            g_vmapping[i].mtime == (uint32_t)st.st_mtime && g_vmapping[i].off == off)
            return g_vmapping[i].start;
    return 0;
}

/* Holds: the address ranges of the saved run's libraries, reserved (no access) when the translation cache is loaded.
   Allocations anywhere avoid them, the guest's as well as the translator's own (the tables of a loaded translation
   cache take space the saved run did not take, and everything allocated after them moves). So a library without a
   fixed address finds its place of the saved run free (jit_cached_image), and a temporary mapping does not land on
   translated pages: Firefox maps libxul whole to read it ahead and unmaps it before dyld loads it; landing on the
   saved libxul it discarded 13,000 blocks, and the translation cache with them. When the guest asks for a range there
   (dyld reserving a library's preferred address), the hold gives way: jit_hold_release. ROSETTA_NO_HOLD */
#define HOLD_N 512
static struct { uint32_t a, end; } g_hold[HOLD_N];
static int g_nhold;
static unsigned long g_st_hold, g_st_hold_given;

/* reserve the free parts of [a, end) */
static void hold_range(uint32_t a, uint32_t end)
{
    a &= ~4095u;
    end = (end + 4095) & ~4095u;
    while (a < end && g_nhold < HOLD_N) {
        vm_address_t r = a, x;
        vm_size_t sz = 0;
        vm_region_basic_info_data_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT;
        mach_port_t obj;
        uint32_t stop = end;
        if (vm_region(mach_task_self(), &r, &sz, VM_REGION_BASIC_INFO, (vm_region_info_t)&info, &cnt, &obj) == KERN_SUCCESS) {
            if ((uint32_t)r <= a) {                     /* a lies in something mapped: behind it */
                if ((uint32_t)(r + sz) <= a) break;
                a = (uint32_t)(r + sz);
                continue;
            }
            if ((uint32_t)r < stop) stop = (uint32_t)r;
        }
        x = a;
        if (vm_allocate(mach_task_self(), &x, stop - a, FALSE) == KERN_SUCCESS) {
            vm_protect(mach_task_self(), x, stop - a, FALSE, VM_PROT_NONE);
            g_hold[g_nhold].a = a;
            g_hold[g_nhold++].end = stop;
            g_st_hold += stop - a;
        }
        a = stop;
    }
}

/* the guest takes [a, a + len) (gone: it has replaced or freed it already, otherwise the hold is freed here) */
void jit_hold_release(uint32_t a, uint32_t len, int gone)
{
    uint32_t end = (uint32_t)(((uint64_t)a + len + 4095 > 0xffffffffu) ? 0xfffff000u : ((uint64_t)a + len + 4095) & ~4095u);
    int i;
    a &= ~4095u;
    for (i = 0; i < g_nhold; i++) {
        uint32_t lo = g_hold[i].a > a ? g_hold[i].a : a, hi = g_hold[i].end < end ? g_hold[i].end : end;
        if (lo >= hi) continue;
        if (!gone) vm_deallocate(mach_task_self(), lo, hi - lo);
        g_st_hold_given += hi - lo;
        if (lo == g_hold[i].a && hi == g_hold[i].end) {           /* all of it */
            g_hold[i--] = g_hold[--g_nhold];
        } else if (lo == g_hold[i].a) g_hold[i].a = hi;
        else if (hi == g_hold[i].end) g_hold[i].end = lo;
        else if (g_nhold < HOLD_N) {                              /* the middle: split */
            g_hold[g_nhold].a = hi;
            g_hold[g_nhold++].end = g_hold[i].end;
            g_hold[i].end = lo;
        } else {                                                  /* no room to split: the rest goes too */
            if (!gone) {
                vm_deallocate(mach_task_self(), g_hold[i].a, lo - g_hold[i].a);
                vm_deallocate(mach_task_self(), hi, g_hold[i].end - hi);
            }
            g_hold[i--] = g_hold[--g_nhold];
        }
    }
}

/* before the translator allocates the tables of the translation cache: they would land where the saved run (which
   had no such tables) put the first libraries */
static void holds_take(int n)
{
    int j;
    if (getenv("ROSETTA_NO_HOLD")) return;
    for (j = 0; j < n; j++)               /* program and dyld are mapped already (main.c) */
        if (g_vimage[j].type != 2 && g_vimage[j].type != 7 && g_vimage[j].start >= RESERVE_END && g_vimage[j].end > g_vimage[j].start)
            hold_range(g_vimage[j].start, g_vimage[j].end);
    if (g_cache_show) fprintf(stderr, "rosetta: %d ranges held for the libraries (%lu KB)\n", g_nhold, g_st_hold >> 10);
}

static void holds_drop(void)
{
    while (g_nhold > 0) {
        g_nhold--;
        vm_deallocate(mach_task_self(), g_hold[g_nhold].a, g_hold[g_nhold].end - g_hold[g_nhold].a);
    }
    g_st_hold = 0;
}

/* guest images (i386 Mach header at the start of the region, below 0x90000000) with their file and the whole region */
static int cache_images(CacheImage *b, int max)
{
    vm_address_t a = 0x1000;
    vm_size_t sz;
    vm_region_basic_info_data_t info;
    mach_msg_type_number_t n;
    mach_port_t obj;
    int nb = 0;
    while (nb < max) {
        uint32_t head[4];
        vm_size_t was_read = 0;
        n = VM_REGION_BASIC_INFO_COUNT;
        if (vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO, (vm_region_info_t)&info, &n, &obj) || a >= 0xffff0000u) break;
        if (a >= 0x90000000u && a < 0xb0000000u) { a = 0xb0000000u; continue; }   /* the host's libraries; guest images
                                                                                     can lie above them (0xc0000000) */
        if ((info.protection & VM_PROT_READ) && sz >= 4096 &&
            vm_read_overwrite(mach_task_self(), a, 16, (vm_address_t)head, &was_read) == KERN_SUCCESS && was_read == 16 &&
            ld32((uint32_t)(uintptr_t)&head[0]) == 0xfeedface && ld32((uint32_t)(uintptr_t)&head[1]) == 7) {
            uint32_t type = ld32((uint32_t)(uintptr_t)&head[3]), ncmds = ld32((uint32_t)a + 16), lc = (uint32_t)a + 28, k;
            uint32_t text = 0, high = 0;
            const char *name = 0;
            struct stat st;
            for (k = 0; k < ncmds && lc + 32 <= (uint32_t)a + (uint32_t)sz; k++) {
                uint32_t cmd = ld32(lc), cgr = ld32(lc + 4);
                if (cmd == 0xd || cmd == 0xf) name = (const char *)(uintptr_t)(lc + ld32(lc + 8));
                if (cmd == 1) {
                    uint32_t va = ld32(lc + 24), vs = ld32(lc + 28);
                    if (!strncmp((const char *)(uintptr_t)(lc + 8), "__TEXT", 16)) text = va;
                    if (strncmp((const char *)(uintptr_t)(lc + 8), "__PAGEZERO", 16) && va + vs > high) high = va + vs;
                }
                if (cgr < 8) break;
                lc += cgr;
            }
            int ok = 0, j;
            if (type == 2) name = g_prog_full[0] ? g_prog_full : 0;
            if (type == 2 || type == 7) {         /* program and dyld are loaded by main.c (dyld from the system): by path */
                if (name && stat(name, &st) == 0) {
                    b[nb].size = (uint32_t)st.st_size; b[nb].mtime = (uint32_t)st.st_mtime; b[nb].ino = (uint32_t)st.st_ino;
                    ok = 1;
                }
            } else {                            /* libraries: the way dyld mapped the file (newest first) */
                for (j = g_nmapping - 1;
                     j >= 0 && ((uint32_t)a < g_mapping[j].start || (uint32_t)a >= g_mapping[j].end || !g_mapping[j].ino); j--) ;
                if (j >= 0) {
                    b[nb].size = g_mapping[j].size; b[nb].mtime = g_mapping[j].mtime; b[nb].ino = g_mapping[j].ino;
                    ok = 1;
                }
            }
            if (ok) {
                b[nb].start = (uint32_t)a;
                b[nb].end = (uint32_t)a + (high - text);
                b[nb].type = type;
                snprintf(b[nb].path, sizeof b[nb].path, "%s", name ? name : "?");
                nb++;
            }
        }
        a += sz;
    }
    return nb;
}

/* guest address in one of the images (or the commpage) */
static int in_images(uint32_t eip, const CacheImage *b, uint32_t n)
{
    uint32_t j;
    if (eip - COMMPAGE_BASE < COMMPAGE_SIZE) return 1;
    for (j = 0; j < n; j++) if (eip >= b[j].start && eip < b[j].end) return 1;
    return 0;
}

static void cache_tidy(void);

static void cache_write(void)
{
    CacheHead k;
    CacheImage *b;
    char tmp[1100];
    uint32_t i, *t, nforeign = 0, n;
    uint8_t *foreign = 0;
    uint32_t **stub = 0;
    int fd, ok = 1;
    cache_path();
    if (g_flush == 2) invalidate_ranges();     /* (the dispatcher normally has done this already) */
    if (g_flush == 1 || g_flush == 3) return;  /* everything is about to be discarded */
    if (g_cache_show && g_cache_path[0] && g_st_flushes) fprintf(stderr, "rosetta: translation cache not written (%lu flushes)\n", g_st_flushes);
    /* loaded and flushed anyway: does not fit this program (file changed, guest changes code pages). Delete it,
       otherwise it is loaded, discarded and never replaced at every start; the next run creates it anew. Not when only
       the code region ran full (Firefox: its startup alone is 30 MB of code) */
    if (g_cache_loaded && g_st_flushes > g_st_full_flushes) unlink(g_cache_path);
    if (!g_cache_path[0] || g_st_flushes || !g_nused || g_free == g_free_loaded) return;
    /* at most g_cache_cap bytes of code: a run that loads it needs room for what it translates anew (JIT code of the
       guest is never in the file, other paths through the program) or it runs full and loses the loaded code */
    if ((uint32_t)((uint8_t *)g_free - (uint8_t *)g_cache) > g_cache_cap) {
        if (g_cache_show) fprintf(stderr, "rosetta: translation cache not written (%u KB code, more than %u KB)\n",
                                  (unsigned)(((uint8_t *)g_free - (uint8_t *)g_cache) >> 10), g_cache_cap >> 10);
        return;
    }
    /* only newly bound jump stubs (writable pages, every run binds anew: old entry removed, new one
       added) are no reason to write; live entries count */
    {
        uint32_t live = 0, before = g_cache_loaded ? (uint32_t)g_st_cache : 0;
        for (i = 0; i < g_nused; i++)
            if (g_map[g_inuse[i]].code && g_map[g_inuse[i]].eip && g_map[g_inuse[i]].eip != 0xffffffffu) live++;
        if (g_written_live > before) before = g_written_live;   /* a checkpoint of this run */
        /* only when there is substantial new code: programs with threads interleave differently each time and bring a
           few new blocks in every run; translating those is faster than rewriting and loading the whole
           translation cache (MB). Or when remapped ranges discarded as many blocks of the translation cache (a library
           at another address): the next run would discard them again. A few discarded blocks in every run (an
           allocation that lands on pages of the saved run) are cheaper to translate again than to rewrite */
        if (before && live < before + 32 + before / 64 && g_st_inval_loaded < g_inval_written + 32 + before / 64) return;
        g_written_live = live;
        g_inval_written = g_st_inval_loaded;
    }
    if (g_cache != (uint32_t *)(uintptr_t)CACHE_ADDR) return;
    b = malloc(sizeof *b * CACHE_IMAGES);
    t = malloc(8 * ((size_t)g_nused + VAR_N * 2 + (1u << 15)) + 8 * (size_t)g_cpool_n + 16 * (size_t)g_vpool_n + 4 * (size_t)VL_PCS + 8 * (size_t)TABLE +
               8 * (size_t)g_nhead + sizeof(BlockDep) * (size_t)g_ndep + sizeof(Mapping) * (size_t)g_nmapping);
    if (!b || !t) { free(b); free(t); return; }
    memset(&k, 0, sizeof k);
    k.magic = CACHE_MAGIC;
    k.cache = CACHE_ADDR;
    k.prolog = (uint32_t)((uint8_t *)g_cache_start - (uint8_t *)g_cache);
    k.code = (uint32_t)((uint8_t *)g_free - (uint8_t *)g_cache);
    k.nimages = (uint32_t)cache_images(b, CACHE_IMAGES);
    /* only code from files (and the commpage) goes into the map: what came from anonymous memory (trampolines that a
       framework generates at run time) may differ or be missing next time. Such blocks stay in the code region, but
       in the written copy their first instruction jumps to an exit stub with their eip, which chains to whatever
       is translated there next time. The same for a block without a dependency record (it could not be discarded
       selectively) */
    nforeign = 0;
    foreign = calloc((size_t)g_nused + 1, 1);
    if (!foreign) { free(b); free(t); return; }
    for (i = 0; i < g_nused; i++) {
        uint32_t eip = g_map[g_inuse[i]].eip;
        BlockDep *d;
        if (!g_map[g_inuse[i]].code || !eip || eip == 0xffffffffu) continue;
        if (!(d = dep_find(g_map[g_inuse[i]].code)) || (d->code & 1) || !in_images(eip, b, k.nimages)) { foreign[i] = 1; nforeign++; }
    }
    if (nforeign) {
        uint32_t *keep = p;
        int wb = g_sl_wb_now;
        if (g_free >= g_half_end[0] || (uint32_t)(g_half_end[0] - g_free) < 16 * nforeign + 64) { free(foreign); free(b); free(t); return; }
        stub = malloc(sizeof *stub * nforeign);
        if (!stub) { free(foreign); free(b); free(t); return; }
        g_sl_wb_now = 0;
        p = g_free;
        for (i = 0, n = 0; i < g_nused; i++)
            if (foreign[i]) { stub[n++] = p; stubby(g_map[g_inuse[i]].eip, 0); }
        sys_icache_invalidate(g_free, (size_t)((uint8_t *)p - (uint8_t *)g_free));
        g_free = p;
        p = keep;
        g_sl_wb_now = wb;
        if (g_cache_show) fprintf(stderr, "rosetta: %u blocks outside the images left out of the translation cache\n", nforeign);
    }
    k.code = (uint32_t)((uint8_t *)g_free - (uint8_t *)g_cache);   /* with the exit stubs */
    k.vlring = (uint32_t)g_vl_nring;
    k.ncpool = (uint32_t)g_cpool_n;
    k.nvpool = (uint32_t)g_vpool_n;
    snprintf(tmp, sizeof tmp, "%s.%d", g_cache_path, (int)getpid());
    if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) { free(foreign); free(stub); free(b); free(t); return; }
    /* map: eip, offset in the cache */
    for (i = 0; i < g_nused; i++) {
        uint32_t h = g_inuse[i];
        if (!g_map[h].code || !g_map[h].eip || g_map[h].eip == 0xffffffffu || foreign[i]) continue;
        t[2 * k.nmap] = g_map[h].eip;
        t[2 * k.nmap + 1] = (uint32_t)((uint8_t *)g_map[h].code - (uint8_t *)g_cache);
        k.nmap++;
    }
    {   /* code pages (index, word), variants (location, target, offset), constants, prefetch sites */
        uint32_t *s = t + 2 * k.nmap;
        for (i = 0; i < (1u << 15); i++) if (g_codepages[i]) { s[2 * k.npages] = i; s[2 * k.npages + 1] = g_codepages[i]; k.npages++; }
        s += 2 * k.npages;
        for (i = 0; i < VAR_N; i++) {
            BlockDep *d;
            if (g_var[i].code && in_images(g_var[i].loc, b, k.nimages) && in_images(g_var[i].target, b, k.nimages) &&
                (d = dep_find(g_var[i].code)) != 0 && !(d->code & 1)) {
                d->code |= 2;               /* its record goes into the file */
                s[3 * k.nvar] = g_var[i].loc; s[3 * k.nvar + 1] = g_var[i].target;
                s[3 * k.nvar + 2] = (uint32_t)((uint8_t *)g_var[i].code - (uint8_t *)g_cache); k.nvar++;
            }
        }
        s += 3 * k.nvar;
        memcpy(s, g_cpool, 8 * (size_t)k.ncpool);
        s += 2 * k.ncpool;
        memcpy(s, g_vpool, 16 * (size_t)k.nvpool);      /* vperm patterns: the code loads them by address */
        s += 4 * k.nvpool;
        for (i = 0; i < VL_PCS; i++) if (g_vl_pc[i]) s[k.nvlpc++] = g_vl_pc[i];
        s += k.nvlpc;
        /* jump table (targets of indirect jumps): otherwise every run fills it again through misses in the dispatcher */
        for (i = 0; i < (g_tab_full ? TABLE : g_tab_n); i++) {
            uint32_t x = g_tab_full ? i : g_tab_slot[i], eip = g_table[2 * x], *code = (uint32_t *)(uintptr_t)g_table[2 * x + 1];
            if (!eip || code < g_cache_start || code >= g_free || !in_images(eip, b, k.nimages)) continue;
            s[2 * k.ntab] = eip;
            s[2 * k.ntab + 1] = (uint32_t)((uint8_t *)code - (uint8_t *)g_cache);
            k.ntab++;
        }
        s += 2 * k.ntab;
        for (i = 0; i < (uint32_t)g_nhead; i++) {     /* loop heads and their exits (halting) */
            s[2 * i] = (uint32_t)((uint8_t *)g_head[i].head - (uint8_t *)g_cache);
            s[2 * i + 1] = (uint32_t)((uint8_t *)g_head[i].exit_ptr - (uint8_t *)g_cache);
        }
        k.nhead = (uint32_t)g_nhead;
        s += 2 * k.nhead;
        /* dependency records of the blocks in the map and of the variants (marked above), in the order of the code */
        for (i = 0; i < g_nused; i++)
            if (g_map[g_inuse[i]].code && g_map[g_inuse[i]].eip && g_map[g_inuse[i]].eip != 0xffffffffu && !foreign[i])
                dep_find(g_map[g_inuse[i]].code)->code |= 2;
        for (i = 0; i < g_ndep; i++)
            if (g_dep[i].code & 2) {
                g_dep[i].code &= ~2u;
                memcpy(s + 5 * k.ndep, &g_dep[i], sizeof g_dep[i]);
                k.ndep++;
            }
        s += 5 * k.ndep;
        k.nmapping = (uint32_t)g_nmapping;      /* the mappings of this run (jit_mapping_expected next time) */
        memcpy(s, g_mapping, sizeof *g_mapping * k.nmapping);
        s += 6 * k.nmapping;
        {   /* code from a page boundary of the file (mmap on load) */
            size_t front = sizeof k + sizeof *b * k.nimages + 4 * (size_t)(s - t);
            static const char null[4096];
            k.codeoffset = (uint32_t)((front + 4095) & ~(size_t)4095);
            size_t len = k.code - k.prolog;
            ok = write(fd, &k, sizeof k) == (ssize_t)sizeof k &&
                 write(fd, b, sizeof *b * k.nimages) == (ssize_t)(sizeof *b * k.nimages) &&
                 write(fd, t, 4 * (size_t)(s - t)) == (ssize_t)(4 * (size_t)(s - t)) &&
                 (k.codeoffset == front || write(fd, null, k.codeoffset - front) == (ssize_t)(k.codeoffset - front)) &&
                 write(fd, g_cache_start, len) == (ssize_t)len;
            /* blocks outside the images start with the exit in the file: the word patched there afterwards (a copy of
               the whole code for these few words cost 14 MB of memcpy and fresh pages at each of Calculator's writes) */
            for (i = 0, n = 0; ok && i < g_nused; i++)
                if (foreign[i]) {
                    uint32_t *at = g_map[g_inuse[i]].code;
                    uint32_t w = B_((uint32_t)(uintptr_t)stub[n] - (uint32_t)(uintptr_t)at);
                    ok = pwrite(fd, &w, 4, (off_t)k.codeoffset + 4 * (off_t)(at - g_cache_start)) == 4;
                    n++;
                }
        }
    }
    if (close(fd) == 0 && ok) rename(tmp, g_cache_path); else unlink(tmp);
    free(foreign);
    free(stub);
    free(b);
    free(t);
    cache_tidy();
}

/* Translation caches and hot lists of other builds of rosetta never fit again, for any program: delete them
   (otherwise the folder grows with every build); names: cache-PROGRAM-BUILD-ENV, hot-PROGRAM-BUILD. Every program
   has a translation cache of its own (a command line program some 100 KB, one with windows 15 to 25 MB): beyond
   ROSETTA_CACHE_MAX_MB together (default 256) the ones used longest ago go */
typedef struct { uint32_t used, size; char name[64]; } CacheFile;

static int used_earlier(const void *x, const void *y)
{
    const CacheFile *a = x, *b = y;
    return a->used < b->used ? -1 : a->used > b->used;
}

static void cache_tidy(void)
{
    char folder[1024], path[1100], *isect, build[16];
    const char *own, *m = getenv("ROSETTA_CACHE_MAX_MB");
    uint64_t total = 0, max = (uint64_t)(m && atoi(m) >= 0 ? atoi(m) : 256) << 20;
    CacheFile *f = 0;
    int nf = 0, maxf = 0, i;
    DIR *d;
    struct dirent *e;
    snprintf(folder, sizeof folder, "%s", g_cache_path);
    if (getenv("ROSETTA_CACHE_FILE") || !(isect = strrchr(folder, '/'))) return;
    *isect = 0;
    own = g_cache_path + (isect + 1 - folder);     /* name of this program's translation cache */
    snprintf(build, sizeof build, "%08x", fnv(__DATE__ " " __TIME__, 2166136261u));
    if (!(d = opendir(folder))) return;
    while ((e = readdir(d)) != 0) {
        const char *n = e->d_name;
        int skip = !strncmp(n, "cache-", 6) ? 15 : !strncmp(n, "hot-", 4) ? 13 : 0;   /* start of BUILD */
        struct stat st;
        if (!skip || strlen(n) < (size_t)skip + 8 || n[skip - 1] != '-') continue;
        snprintf(path, sizeof path, "%s/%s", folder, n);
        if (strncmp(n + skip, build, 8)) { unlink(path); continue; }
        if (skip != 15 || strlen(n) >= sizeof f->name || !strcmp(n, own) || stat(path, &st)) continue;
        if (nf == maxf) {
            void *z = realloc(f, (size_t)(maxf = maxf ? 2 * maxf : 256) * sizeof *f);
            if (!z) break;
            f = z;
        }
        f[nf].used = (uint32_t)(st.st_atime > st.st_mtime ? st.st_atime : st.st_mtime);
        f[nf].size = (uint32_t)st.st_size;
        snprintf(f[nf].name, sizeof f[nf].name, "%s", n);
        total += st.st_size;
        nf++;
    }
    closedir(d);
    {
        struct stat st;
        if (stat(g_cache_path, &st) == 0) total += st.st_size;     /* the own one always stays */
    }
    if (total > max && nf) {
        qsort(f, (size_t)nf, sizeof *f, used_earlier);
        for (i = 0; i < nf && total > max; i++) {
            snprintf(path, sizeof path, "%s/%s", folder, f[i].name);
            if (unlink(path) == 0) total -= f[i].size;
        }
        if (g_cache_show) fprintf(stderr, "rosetta: %d translation caches used longest ago deleted (together over %u MB)\n",
                                  i, (unsigned)(max >> 20));
    }
    free(f);
}

/* at startup (after jit_create, before the first block): read the translation cache if it fits this run */
static void cache_load(void)
{
    CacheHead k;
    uint32_t i, n, *t = 0;
    int fd, j;
    const char *reason = "header";
    uint64_t t0 = mach_absolute_time();
    int f0 = jit_pagefault();
    g_free_loaded = g_free;
    cache_path();
    if (getenv("ROSETTA_CACHE_SHOW")) g_cache_show = atoi(getenv("ROSETTA_CACHE_SHOW"));
    /* the first half (without its pool): a run that loads the file goes on in the second */
    if (getenv("ROSETTA_CACHE_CAP_MB")) g_cache_cap = (uint32_t)atoi(getenv("ROSETTA_CACHE_CAP_MB")) << 20;
    if (!g_cache_cap || g_cache_cap > (uint32_t)((uint8_t *)g_half_end[0] - (uint8_t *)g_cache))
        g_cache_cap = (uint32_t)((uint8_t *)g_half_end[0] - (uint8_t *)g_cache);
    if (g_cache_cap < (1u << 20)) g_cache_cap = 1u << 20;
    if (g_cache_path[0]) g_cap_at = g_cache_cap - (256u << 10);
    if (g_cache_show)
        fprintf(stderr, "rosetta: translation cache %s, cache %08x, prologue %u\n", g_cache_path[0] ? g_cache_path : "(no path)",
                (unsigned)(uintptr_t)g_cache, (unsigned)((uint8_t *)g_cache_start - (uint8_t *)g_cache));
    if (!g_cache_path[0] || g_cache != (uint32_t *)(uintptr_t)CACHE_ADDR) return;
    if ((fd = open(g_cache_path, O_RDONLY)) < 0) return;
    if (read(fd, &k, sizeof k) != (ssize_t)sizeof k || k.magic != CACHE_MAGIC || k.cache != CACHE_ADDR ||
        k.prolog != (uint32_t)((uint8_t *)g_cache_start - (uint8_t *)g_cache) || k.code < k.prolog ||
        k.code > (uint32_t)((uint8_t *)g_half_end[0] - (uint8_t *)g_cache) || k.nimages > CACHE_IMAGES || k.ncpool > CPOOL || k.nvar > VAR_N ||
        k.npages > (1u << 15) || k.nvlpc > VL_PCS || k.vlring > VL_RINGS || k.ntab > TABLE || k.nhead > (1u << 20) ||
        k.ndep > (1u << 22) || k.nmapping > MAPPING_N || k.nvpool > VPOOL)
        goto way;
    if (read(fd, g_vimage, sizeof *g_vimage * k.nimages) != (ssize_t)(sizeof *g_vimage * k.nimages)) goto way;
    /* check program and dyld (rosetta loads it itself) now; the libraries only when dyld maps them
       (jit_mapping_expected, fstat on the open descriptor): ~25 us per stat, Foundation has 107 images */
    for (j = 0; j < (int)k.nimages; j++) {
        struct stat st;
        g_vimage[j].path[sizeof g_vimage[j].path - 1] = 0;
        if (g_vimage[j].type != 2 && g_vimage[j].type != 7) continue;
        if (stat(g_vimage[j].path, &st) || (uint32_t)st.st_size != g_vimage[j].size || (uint32_t)st.st_mtime != g_vimage[j].mtime ||
            (uint32_t)st.st_ino != g_vimage[j].ino) {
            reason = g_vimage[j].path;
            goto way;
        }
        /* the saved program must also be this one: a cache file named by ROSETTA_CACHE_FILE serves any program (openssl's
           blocks were taken for gzip's code at the same addresses) */
        if (g_vimage[j].type == 2 && (!g_prog_full[0] || stat(g_prog_full, &st) || (uint32_t)st.st_ino != g_vimage[j].ino ||
                                      (uint32_t)st.st_size != g_vimage[j].size)) {
            reason = "another program";
            goto way;
        }
    }
    reason = "read";
    holds_take((int)k.nimages);
    n = 2 * k.nmap + 2 * k.npages + 3 * k.nvar + 2 * k.ncpool + 4 * k.nvpool + k.nvlpc + 2 * k.ntab + 2 * k.nhead + 5 * k.ndep + 6 * k.nmapping;
    if (!(t = malloc(4 * (size_t)n + 4)) || read(fd, t, 4 * (size_t)n) != (ssize_t)(4 * (size_t)n) || (k.codeoffset & 4095) ||
        k.codeoffset < sizeof k + sizeof *g_vimage * k.nimages + 4 * (size_t)n) goto way;
    /* map the code instead of copying it: no zero pages, no copy, only touched pages cost. The kernel syncs the
       caches when it maps a page for the first time (vm_fault_enter, pmap_sync_page_data_phys); writing
       (chaining) copies the page (MAP_PRIVATE). ROSETTA_CACHE_READ: copy as before */
    if (getenv("ROSETTA_CACHE_READ") ||
        mmap(g_cache_start, (k.code - k.prolog + 4095) & ~4095u, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED | MAP_PRIVATE,
             fd, (off_t)k.codeoffset) != (void *)g_cache_start) {
        if (pread(fd, g_cache_start, k.code - k.prolog, (off_t)k.codeoffset) != (ssize_t)(k.code - k.prolog)) goto way;
        sys_icache_invalidate(g_cache_start, k.code - k.prolog);
    }
    close(fd);
    g_nvimage = (int)k.nimages;
    g_free = g_cache + k.code / 4;
    {   /* the map empty in the right size right away (map_grow would rehash several times and grow fourfold: 2^18
           entries instead of 2^17 for Calculator's 60,000 blocks, 1.5 MB more pages to touch) */
        uint32_t bits = g_map_bits;
        while (2 * (k.nmap + 1) > (1u << bits)) bits++;
        if (bits > g_map_bits && !g_nused) {
            void *m = calloc(1u << bits, sizeof *g_map), *u = calloc(1u << bits, sizeof *g_inuse);
            if (m && u) {
                free(g_map); free(g_inuse);
                g_map = m; g_inuse = u;
                g_map_bits = bits; g_map_mask = (1u << bits) - 1;
            } else { free(m); free(u); }
        }
    }
    while (2 * (k.nmap + 1) > g_map_mask + 1) map_grow();
    for (i = 0; i < k.nmap; i++) map_add(t[2 * i], g_cache + t[2 * i + 1] / 4);
    {
        uint32_t *s = t + 2 * k.nmap;
        for (i = 0; i < k.npages; i++) g_codepages[s[2 * i] & 0x7fff] = s[2 * i + 1];
        s += 2 * k.npages;
        for (i = 0; i < k.nvar; i++) {
            uint32_t h = ((s[3 * i] ^ (s[3 * i + 1] * 31u)) * 2654435761u) >> 23;
            while (g_var[h].code) h = (h + 1) & (VAR_N - 1);
            g_var[h].loc = s[3 * i]; g_var[h].target = s[3 * i + 1]; g_var[h].code = g_cache + s[3 * i + 2] / 4;
        }
        s += 3 * k.nvar;
        for (i = 0; i < k.ncpool; i++) { uint64_t v; memcpy(&v, s + 2 * i, 8); cpool(v); }
        s += 2 * k.ncpool;
        memcpy(g_vpool, s, 16 * (size_t)k.nvpool);      /* same places as in the saved run */
        g_vpool_n = (int)k.nvpool;
        s += 4 * k.nvpool;
        for (i = 0; i < k.nvlpc; i++) vl_pc_add(s[i]);
        g_vl_nring = (int)k.vlring;
        s += k.nvlpc;
        for (i = 0; i < k.ntab; i++)
            if (s[2 * i + 1] >= k.prolog && s[2 * i + 1] < k.code) table_add(s[2 * i], g_cache + s[2 * i + 1] / 4);
        s += 2 * k.ntab;
        for (i = 0; i < k.nhead; i++)
            if (s[2 * i] >= k.prolog && s[2 * i] < k.code && s[2 * i + 1] >= k.prolog && s[2 * i + 1] < k.code)
                head_note(g_cache + s[2 * i] / 4, g_cache + s[2 * i + 1] / 4);
        s += 2 * k.nhead;
        /* dependency records: ascending offsets inside the code, otherwise remapping flushes everything as before */
        if (k.ndep > g_dep_max) {
            void *z = realloc(g_dep, (size_t)k.ndep * sizeof *g_dep);
            if (z) { g_dep = z; g_dep_max = k.ndep; }
        }
        if (k.ndep <= g_dep_max) {
            memcpy(g_dep, s, (size_t)k.ndep * sizeof *g_dep);
            g_ndep = k.ndep;
            for (i = 0; i < k.ndep; i++)
                if ((g_dep[i].code & 3) || g_dep[i].code < k.prolog || g_dep[i].code >= k.code ||
                    (i && g_dep[i].code <= g_dep[i - 1].code)) { g_dep_lost = 1; break; }
        } else g_dep_lost = 1;
        s += 5 * k.ndep;
        memcpy(g_vmapping, s, sizeof *g_vmapping * k.nmapping);
        g_nvmapping = (int)k.nmapping;
    }
    free(t);
    g_cache_loaded = 1;
    g_st_cache = k.nmap;
    g_cache_time = mach_absolute_time() - t0;
    g_cache_error = jit_pagefault() - f0;
    g_free_loaded = g_free;
    if (g_cache_show) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        fprintf(stderr, "rosetta: translation cache loaded: %u blocks, %u KB code, %u images, %.1f ms, %d page faults\n", k.nmap,
                (k.code - k.prolog) >> 10, k.nimages, (double)g_cache_time * tb.numer / tb.denom / 1e6, g_cache_error);
    }
    /* with a translation cache, translating costs only once: translate cold code right away, it goes into the next translation cache */
    if (!getenv("ROSETTA_THRESHOLD"))
        g_threshold = getenv("ROSETTA_CACHE_THRESHOLD") ? atoi(getenv("ROSETTA_CACHE_THRESHOLD")) : 1;
    return;
way:
    if (getenv("ROSETTA_CACHE_SHOW")) fprintf(stderr, "rosetta: translation cache does not match (%s)\n", reason);
    close(fd);
    free(t);
    holds_drop();
    g_free = g_cache_start;
}

/* fstat of the descriptor checked last (dyld maps a file in 4-5 pieces); sys.c forgets it on
   close/dup2/open */
void jit_fd_off(int fd) { if (fd == g_fs_fd) g_fs_fd = -1; }

/* sys.c, mmap with MAP_FIXED (fd -1: anonymous): if dyld maps a file exactly as in the run that wrote the
   translation cache (same file unchanged in size, modification time and inode number, the same part of it at this
   address), the code from the translation cache stays valid. Otherwise 0: the blocks that rely on these pages are
   discarded before code from them runs. Every mapping is recorded for the next translation cache */
int jit_mapping_expected(uint32_t a, uint32_t len, int fd, uint32_t off)
{
    int j;
    Mapping m;
    if (!g_cache_path[0]) return 0;
    memset(&m, 0, sizeof m);
    m.start = a;
    m.end = a + len;
    if (fd >= 0) {
        if (fd != g_fs_fd) {
            if (fstat(fd, &g_fs)) return 0;
            g_fs_fd = fd;
        }
        m.ino = (uint32_t)g_fs.st_ino ? (uint32_t)g_fs.st_ino : 1;
        m.size = (uint32_t)g_fs.st_size;
        m.mtime = (uint32_t)g_fs.st_mtime;
        m.off = off;
    }
    if (g_nmapping < MAPPING_N) g_mapping[g_nmapping++] = m;
    if (!g_cache_loaded || !m.ino) return 0;
    /* code translated in this run relies on these pages: it came from what was mapped here before, not from the
       mapping of the saved run (a file mapped over another one and back again) */
    if (pages_hit_in(g_newpages, a, len)) return 0;
    /* the newest mapping of the saved run that covers the range: was the same part of the same file there? */
    for (j = g_nvmapping - 1; j >= 0 && (a < g_vmapping[j].start || m.end > g_vmapping[j].end); j--) ;
    if (j >= 0 && g_vmapping[j].ino == m.ino && g_vmapping[j].size == m.size && g_vmapping[j].mtime == m.mtime &&
        off - g_vmapping[j].off == a - g_vmapping[j].start) {
        g_st_expected++;
        return 1;
    }
    if (g_cache_show) {
        const char *name = "?";
        int i;
        for (i = 0; i < g_nvimage; i++) if (a >= g_vimage[i].start && a < g_vimage[i].end) name = g_vimage[i].path;
        if (j < 0) fprintf(stderr, "rosetta: mapping %08x+%x: none like it in the translation cache (%s)\n", a, len, name);
        else fprintf(stderr, "rosetta: mapping %08x+%x: file differs from the translation cache (%s: inode %u/%u, size %u/%u, "
                     "offset %x/%x)\n", a, len, name, m.ino, g_vmapping[j].ino, m.size, g_vmapping[j].size, off,
                     g_vmapping[j].off + (a - g_vmapping[j].start));
    }
    return 0;
}

/* sys.c, mmap without MAP_FIXED (also vm_allocate): with a loaded translation cache, pages there can be marked as
   holding code of the old run (a library that dyld has not mapped yet this time); then flush */
void jit_new_mapped(uint32_t a, uint32_t len)
{
    if (g_cache_loaded && pages_hit(a, len)) jit_range_changed(a, len, "new mapping");
}

/* at the end of the run: write the previous list and the blocks translated now (merged) if something new was added */
static void jit_end_hot(void);
void jit_end(void)
{
    if (g_patched) unpatch();
    cache_write();
    jit_end_hot();
}

static void jit_end_hot(void)
{
    uint32_t *t, n = 0, i, k, fresh = 0;
    char tmp[1100], folder[1024], *s;
    int fd;
    if (!g_hot_path[0] || !g_cache || g_threshold <= 1) return;
    t = malloc(4 * ((size_t)g_hot_n + g_nused + 1));
    if (!t) return;
    for (i = 0; i < g_hot_n; i++) t[n++] = g_hot[i];
    for (i = 0; i < g_nused; i++) {
        uint32_t h = g_inuse[i];
        if (g_map[h].code && g_map[h].eip && g_map[h].eip != 0xffffffffu) t[n++] = g_map[h].eip;
    }
    qsort(t, n, 4, u32_cmp);
    for (i = k = 0; i < n; i++) if (!k || t[i] != t[k - 1]) t[k++] = t[i];
    fresh = k - (g_hot_n < k ? g_hot_n : k);
    if (!fresh || k > HOT_MAX) { free(t); return; }
    snprintf(folder, sizeof folder, "%s", g_hot_path);
    if ((s = strrchr(folder, '/'))) { *s = 0; mkdir(folder, 0755); }
    snprintf(tmp, sizeof tmp, "%s.%d", g_hot_path, (int)getpid());
    if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0) {
        uint32_t head[2] = { 0x48454953u, k };
        if (write(fd, head, 8) == 8 && write(fd, t, 4 * (size_t)k) == (ssize_t)(4 * (size_t)k) && close(fd) == 0) rename(tmp, g_hot_path);
        else { close(fd); unlink(tmp); }
    }
    free(t);
}

/* Interpret up to the next jump (or up to a translated block); if a ret returned to a call site
   whose call was translated, its shadow entry is removed */
static int cold_run(Cpu *c)
{
    int n, jumped, r;
    g_st_cold++;
    /* straight on without a look into the map (cost one cache miss per instruction, 7 % of startup; at the next jump
       execution goes into the dispatcher anyway) */
    r = steps(c, 256, &n, &jumped, &g_flush);
    g_st_cold_insn += (unsigned long)n;
    if (r == RUN_SIGNAL) return RUN_OK;     /* exception for the guest handler: the dispatcher delivers it */
    if (r != RUN_OK || c->finished) return r;
    if (jumped) {
        uint32_t before = (c->shadow & ~(64u * 1024 * 8 - 1)) | ((c->shadow - 8) & (64u * 1024 * 8 - 1));
        if (*(uint32_t *)(uintptr_t)before == c->eip) c->shadow = before;
    }
    return r;
}

static int dispatcher(Cpu *c);

/* Translation cache checkpoint: a program that never ends normally (programs with windows closed by a signal or at
   logout, crashes) would otherwise start without its translations every time. At 6 s and at 45 s of run time the
   cache is written as at the end (only if enough is new). The dispatcher holds the thread lock; the watchdog patches
   nothing while no thread is in translated code. ROSETTA_NO_CHECKPOINT=1 */
static uint64_t g_run_t0, g_checkpoint_at[2];
static int g_checkpoint_n = -1;
volatile int g_checkpoint_idle;         /* mach_msg.c: the main thread waited long for a message (startup is over) */
static int g_checkpoint_idle_done;
static void jit_end_hot(void);

/* first idle moment: the whole startup is translated. Called with the thread lock held, either at the dispatcher or
   by the watchdog while every guest thread waits (thread.c, g_checkpoint_fn) */
static void checkpoint_idle(void)
{
    if (!g_checkpoint_idle || g_checkpoint_idle_done) return;
    g_checkpoint_idle = 0;
    g_checkpoint_idle_done = 1;
    if (getenv("ROSETTA_STATS")) {           /* what the startup cost (programs with windows rarely end normally) */
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        fprintf(stderr, "rosetta: first idle moment after %.2f s: processor %.2f s user, %.2f s system; %ld page faults "
                "(%ld from disk), %ld + %ld context switches\n", (double)(mach_absolute_time() - g_run_t0) * tb.numer / tb.denom / 1e9,
                ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6, ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6, ru.ru_minflt,
                ru.ru_majflt, ru.ru_nvcsw, ru.ru_nivcsw);
        {
            task_events_info_data_t ev;
            mach_msg_type_number_t en = TASK_EVENTS_INFO_COUNT;
            memset(&ev, 0, sizeof ev);
            task_info(mach_task_self(), TASK_EVENTS_INFO, (task_info_t)&ev, &en);
            fprintf(stderr, "rosetta: page faults %d (copy on write %d, from disk %d), Mach messages sent %d, received %d, "
                    "system calls %d + %d, context switches %d\n", ev.faults, ev.cow_faults, ev.pageins, ev.messages_sent,
                    ev.messages_received, ev.syscalls_unix, ev.syscalls_mach, ev.csw);
        }
        if (g_main_cpu && g_main_cpu->host_thread) {   /* the main thread alone (the others mostly wait) */
            thread_basic_info_data_t ti;
            mach_msg_type_number_t tn = THREAD_BASIC_INFO_COUNT;
            mach_port_t th = pthread_mach_thread_np((pthread_t)g_main_cpu->host_thread);
            if (thread_info(th, THREAD_BASIC_INFO, (thread_info_t)&ti, &tn) == KERN_SUCCESS)
                fprintf(stderr, "rosetta: main thread: processor %.2f s user, %.2f s system\n",
                        ti.user_time.seconds + ti.user_time.microseconds / 1e6, ti.system_time.seconds + ti.system_time.microseconds / 1e6);
        }
        thread_stats();
        sys_stats();
        mach_msg_stats();
        thunk_stats();
        interp_op_show();
        fprintf(stderr, "rosetta: own Mach calls: %lu vm_region (page protections); %lu read probes failed (predictions)\n",
                g_st_region, g_st_safe_read);
    }
    if (getenv("ROSETTA_NO_CHECKPOINT") || !g_cache) return;
    if (g_patched) unpatch();
    cache_write();
    jit_end_hot();
    if (g_cache_show) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        fprintf(stderr, "rosetta: checkpoint at the first idle moment (%.1f s): %lu blocks from the translation cache, "
                "%lu translated, %lu flushes, %lu discarded for %lu ranges (%lu from the cache), %lu instructions interpreted cold\n",
                (double)(mach_absolute_time() - g_run_t0) * tb.numer / tb.denom / 1e9, g_st_cache, g_st_blocks, g_st_flushes,
                g_st_inval_blocks, g_st_inval_ranges, g_st_inval_loaded, g_st_cold_insn);
    }
}

/* the code is about to grow beyond what the translation cache file may hold (g_cache_cap): write it now, with the
   code translated first (the startup); later writes of this run find too much code and leave it */
static void checkpoint_cap(void)
{
    g_cap_at = 0;
    if (getenv("ROSETTA_NO_CHECKPOINT") || !g_cache) return;
    if (g_patched) unpatch();
    cache_write();
    jit_end_hot();
    if (g_cache_show) fprintf(stderr, "rosetta: checkpoint at %u KB code (%lu blocks translated)\n",
                              (unsigned)(((uint8_t *)g_free - (uint8_t *)g_cache) >> 10), g_st_blocks);
}

static void checkpoint_maybe(void)
{
    uint64_t now = mach_absolute_time();
    checkpoint_idle();
    if (g_checkpoint_n < 0) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        g_checkpoint_at[0] = g_run_t0 + 6000000000ull * tb.denom / tb.numer;
        g_checkpoint_at[1] = g_run_t0 + 45000000000ull * tb.denom / tb.numer;
        g_checkpoint_n = getenv("ROSETTA_NO_CHECKPOINT") ? 2 : 0;
    }
    if (g_checkpoint_n >= 2 || now < g_checkpoint_at[g_checkpoint_n]) return;
    g_checkpoint_n++;
    if (g_patched) unpatch();
    cache_write();
    jit_end_hot();
    if (g_cache_show) fprintf(stderr, "rosetta: checkpoint %d after %.1f s\n", g_checkpoint_n,
                              (double)(mach_absolute_time() - g_run_t0) / (double)(g_checkpoint_at[0] - g_run_t0) * 6.0);
}

/* one interpreted instruction at the dispatcher: an exception for the guest handler (RUN_SIGNAL) is delivered at the
   start of the next round */
static inline int dstep(Cpu *c)
{
    int r = step(c);
    return r == RUN_SIGNAL ? RUN_OK : r;
}

/* register the thread (ring, swap patterns of the Cpu) */
static void thread_setup(Cpu *c, uint32_t ring)
{
    int o, j;
    c->shadow_base = ring;
    if (!c->shadow) c->shadow = ring;
    for (o = 0; o < 16; o++)                /* swap pattern: output byte j (host byte order) = Intel byte (length - 1 - j) */
        for (j = 0; j < 16; j++) {
            c->vswap8[o][j] = (uint8_t)(j < 8 ? o + 7 - j : o + 7);
            c->vswap4[o][j] = (uint8_t)(j < 4 ? o + 3 - j : o + 3);
            c->vswapp[o][j] = (uint8_t)(j < 8 ? o + 7 - j : o + 23 - j);
        }
    if (g_nfcpu < (int)(sizeof g_fcpu / sizeof g_fcpu[0])) g_fcpu[g_nfcpu++] = c;
    g_cpu = c;
}

/* thread.c: this thread now holds the lock (translation sees its registers) */
void jit_thread_active(Cpu *c) { g_cpu = c; }

/* secondary thread: own return ring, then like the main thread */
static uint32_t g_ring_free[64];        /* rings of finished threads */
static int g_nfree;

int jit_thread_run(Cpu *c)
{
    uint32_t ring = g_nfree ? g_ring_free[--g_nfree] : ring_new();
    if (!ring) { fprintf(stderr, "rosetta: no space for the return ring of a thread\n"); return run(c); }
    thread_setup(c, ring);
    return dispatcher(c);
}

int jit_thread_cont(Cpu *c)
{
    c->shadow = c->shadow_base;        /* new start: old entries are not valid */
    return dispatcher(c);
}

void jit_thread_unregister(Cpu *c)
{
    int i;
    for (i = 0; i < g_nfcpu; i++)
        if (g_fcpu[i] == c) { g_fcpu[i] = g_fcpu[--g_nfcpu]; break; }
    /* keep the ring for the next thread (sentinel entry stays valid) */
    if (c->shadow_base && c->shadow_base != g_shadow_base && g_nfree < (int)(sizeof g_ring_free / sizeof g_ring_free[0]))
        g_ring_free[g_nfree++] = c->shadow_base;
    if (g_cpu == c) g_cpu = 0;
}

int jit_run(Cpu *c)
{
    g_run_t0 = mach_absolute_time();
    g_checkpoint_fn = checkpoint_idle;
    if (getenv("ROSETTA_THRESHOLD")) g_threshold = atoi(getenv("ROSETTA_THRESHOLD"));
    {
        int f0 = jit_pagefault();
        if (jit_create()) { fprintf(stderr, "rosetta: no space for the translator\n"); return run(c); }
        g_create_error = jit_pagefault() - f0;
    }
    hot_load();
    thread_setup(c, g_shadow_base);
    return dispatcher(c);
}

/* Memory fault of guest code (main.c memfault, host signal handler) at host pc: the guest state for the guest's
   handler. In a translated block the guest registers are in r14..r21 (R()), the exact instruction is not known: eip
   = start of the block, flags, deferred stack slots and floating-point values as last written back. 0: approximate
   (not resumable at the same place), 1: exact (the interpreter between instructions), -1: not a fault of guest code
   (the translator itself, a system call) */
static int in_code_cache(uint32_t pc)
{
    return g_cache && pc >= (uint32_t)(uintptr_t)g_cache_start && pc < (uint32_t)(uintptr_t)g_free;
}

/* guest eip of the block whose code contains pc (records in code order); 0: none */
static uint32_t block_of(uint32_t pc)
{
    uint32_t off = pc - (uint32_t)(uintptr_t)g_cache, lo = 0, hi = g_ndep;
    while (lo < hi) {                       /* last record that starts at or before pc */
        uint32_t m = (lo + hi) / 2;
        if ((g_dep[m].code & ~3u) <= off) lo = m + 1; else hi = m;
    }
    return lo ? g_dep[lo - 1].eip : 0;
}

/* host word without a signal on bad memory (fault handler) */
static int host_word(uint32_t a, uint32_t *w)
{
    vm_size_t n = 0;
    return !(a & 3) && vm_read_overwrite(mach_task_self(), a, 4, (vm_address_t)(uintptr_t)w, &n) == KERN_SUCCESS && n == 4;
}

int jit_fault_state(Cpu *c, void *ctx)
{
    ucontext_t *u = (ucontext_t *)ctx;
#if __DARWIN_UNIX03
    uint32_t pc = u->uc_mcontext->__ss.__srr0, lr = u->uc_mcontext->__ss.__lr, *gpr = (uint32_t *)&u->uc_mcontext->__ss.__r0;
#else
    uint32_t pc = u->uc_mcontext->ss.srr0, lr = u->uc_mcontext->ss.lr, *gpr = (uint32_t *)&u->uc_mcontext->ss.r0;
#endif
    if (in_code_cache(pc)) {
        uint32_t e = block_of(pc);
        int i;
        for (i = 0; i < 8; i++) c->r[i] = gpr[R(i)];
        c->shadow = gpr[RSHADOW];
        if (e) c->eip = e;
        return 0;
    }
    if (g_in_code) {
        /* native function or helper called from a block (a native strlen with a bad pointer): registers as last stored,
           eip = the block that called it (return address in lr or in the frame chain of the host stack) */
        uint32_t fp = gpr[1], w, back;
        int depth;
        if (in_code_cache(lr)) { if (block_of(lr)) c->eip = block_of(lr); return 0; }
        for (depth = 0; depth < 16 && host_word(fp, &back) && back > fp; depth++, fp = back)
            if (host_word(back + 8, &w) && in_code_cache(w)) { if (block_of(w)) c->eip = block_of(w); break; }
        return 0;
    }
    return g_guest_exec ? 1 : -1;
}

static int dispatch_loop(Cpu *c);

/* the dispatcher of a guest thread; a memory fault for the guest's handler comes back here (main.c memfault) and
   is delivered at the next round */
static int dispatcher(Cpu *c)
{
    jmp_buf jb;
    void *prev = c->fault_jmp;
    int r;
    if (_setjmp(jb)) {
        g_in_code = 0;
        while (g_patch_wants) sched_yield();     /* the watchdog was patching block starts: let it finish */
        interp_fault_landing(c);
    }
    c->fault_jmp = (void *)jb;
    c->host_thread = (void *)pthread_self();
    /* AltiVec in Java mode (VSCR.NJ = 0): denormal floats as in IEEE and SSE, Mac OS X starts threads with NJ = 1 */
    __asm__ volatile(".long 0x1000038c\n\t.long 0x10000644");     /* vspltisw v0,0; mtvscr v0 */
    r = dispatch_loop(c);
    c->fault_jmp = prev;
    return r;
}

static int dispatch_loop(Cpu *c)
{
    int r;
    for (;;) {
        uint32_t *code, eip;
        g_dispatcher_rounds++;
        if ((!(g_dispatcher_rounds & 0x3fff) && g_checkpoint_n < 2) || g_checkpoint_idle) checkpoint_maybe();
        if (g_cap_at && (uint32_t)((uint8_t *)g_free - (uint8_t *)g_cache) >= g_cap_at) checkpoint_cap();
        if (g_patched) unpatch();       /* restore block starts (jit_interrupt) */
        if (g_state_wish) state_short(c);
        if (g_thread_waiting | c->yield) thread_yield(c);  /* other guest threads (thread.c) */
        if (g_flush) { if (g_flush == 2) invalidate_ranges(); else if (g_flush == 3) flush_full(); else flush(); }
        if (g_sig_pending & ~g_sig_mask) {   /* signal to the guest: build the stack frame, continue in the trampoline */
            sig_deliver(c);
            if (c->finished) return RUN_END;
        }
        eip = c->eip;
        if (g_vt_on) vt_tally(eip, c->jit_kind > KIND_INVALID ? 3 : c->jit_kind);
        if (eip - COMMPAGE_BASE < COMMPAGE_SIZE && !commpage_stub(eip)) {
            uint32_t before;
            r = dstep(c);
            if (r != RUN_OK || c->finished) return c->finished ? RUN_END : r;
            /* commpage routine has returned (without a ret in translated code): remove its shadow entry */
            before = (c->shadow & ~(64u * 1024 * 8 - 1)) | ((c->shadow - 8) & (64u * 1024 * 8 - 1));
            if (*(uint32_t *)(uintptr_t)before == c->eip) c->shadow = before;
            continue;
        }
        code = 0;
        if (c->var_target) {                  /* exit stub of a variant (it is already chained, see below) */
            code = variant(eip, c->var_target);
            c->var_target = 0;
        }
        if (code) ;
        else if (!(code = map_find(eip))) {
            /* still cold (an exit stub counted right before and did not translate, or too rare): interpret
               (never for functions with a native version, see often_enough) */
            int cold = g_threshold > 1 && (g_counted == eip || !often_enough(eip));
            g_counted = 0;
            if (cold) {
                r = cold_run(c);
                if (r != RUN_OK || c->finished) return c->finished ? RUN_END : r;
                continue;
            }
            code = translate(eip);
            if (code && c->jit_kind == KIND_NORMAL) table_add(eip, code);    /* came via an indirect jump */
        } else if (c->jit_kind == KIND_NORMAL) {
            /* indirect jump missed the table (not entered yet or evicted): add it */
            g_st_table_miss++;
            table_add(eip, code);
        }
        if (!code) {
            g_st_interp++;
            interp_tally(c->eip);
            r = dstep(c);
            if (r != RUN_OK || c->finished) return c->finished ? RUN_END : r;
            continue;
        }
        g_in_code = 1;
        g_entry(c, code);
        g_in_code = 0;
        while (g_patch_wants) sched_yield();     /* the watchdog is writing right now: let it finish */
        if (c->jit_kind == KIND_INTERRUPT) {   /* stopped block start: find its guest eip, then restore it */
            c->eip = interrupt_eip(c->eip);
            unpatch();
        }
        if (c->spec_miss) {                 /* target prediction of a block was wrong: translate again without prediction */
            uint32_t s = c->spec_miss, *prev;
            unsigned long l = g_st_flushes;
            c->spec_miss = 0;
            g_st_before_miss++;
            if (g_vt_on) fprintf(stderr, "rosetta: prediction in block %08x wrong, target %08x\n", s, c->eip);
            /* learn the target at the call site; too many (or not inlinable): without prediction */
            if (!c->spec_loc || g_no_variants || !targets_note(c->spec_loc, c->eip)) no_spec_note(s);
            c->spec_loc = 0;
            if ((prev = map_find(s)) != 0) {
                uint32_t *fresh;
                map_remove(s);
                fresh = translate(s);
                if (fresh && l == g_st_flushes) redirect(prev, fresh);
            }
        }
        if (c->jit_kind == KIND_INTERPRET) {
            g_st_interp++;
            interp_tally(c->eip);
            r = dstep(c);
            if (r != RUN_OK || c->finished) return c->finished ? RUN_END : r;
        } else if (c->jit_kind == KIND_INTERRUPT) {
            ;                               /* done above */
        } else if (c->jit_kind == KIND_INVALID) {
            /* block was modified (dyld rewrites jump stubs): translate again, redirect the old block
               so chained jumps to it reach the new one */
            uint32_t *prev = map_find(c->eip), *fresh;
            unsigned long l = g_st_flushes;
            if (prev && prev < g_free_loaded && once_invalid(c->eip)) {
                /* block from the translation cache: the jump stub is still unbound (call into the dyld binder) and will be
                   bound again the same way as in the saved run (libraries at fixed addresses). Interpret the call
                   once and keep the block; afterwards its bytes match (otherwise translate twice and redirect the
                   old block, which copies mapped pages) */
                g_st_inval_cache++;
                r = dstep(c);
                if (r != RUN_OK || c->finished) return c->finished ? RUN_END : r;
            } else {
                g_st_invalid++;
                map_remove(c->eip);
                fresh = translate(c->eip);
                if (prev && fresh && l == g_st_flushes) redirect(prev, fresh);
            }
        } else if (c->jit_kind == KIND_WATCHDOG) {
            /* predicted register had a different value: block without prediction, the old block redirects there */
            uint32_t *prev = map_find(c->eip), *fresh;
            unsigned long l = g_st_flushes;
            g_st_watchdog_miss++;
            no_spec_note(c->eip);
            map_remove(c->eip);
            fresh = translate(c->eip);
            if (prev && fresh && l == g_st_flushes) redirect(prev, fresh);
        } else if (c->jit_kind == KIND_RET) {
            /* the access could have hit the stack slots: block with immediate write-back, the old one redirects there */
            uint32_t *prev = map_find(c->eip), *fresh;
            unsigned long l = g_st_flushes;
            g_st_ret_miss++;
            if (g_vt_on) fprintf(stderr, "rosetta: check at loop entry at %08x failed\n", c->eip);
            no_ret_note(c->eip);
            map_remove(c->eip);
            fresh = translate(c->eip);
            if (prev && fresh && l == g_st_flushes) redirect(prev, fresh);
        } else if (c->jit_kind > KIND_WATCHDOG && (c->jit_kind & 3) == 2) {
            /* switch table of a block: translate the target and write its host address into the entry */
            uint32_t *ent = (uint32_t *)(uintptr_t)(c->jit_kind & ~3u), *target = 0;
            unsigned long l = g_st_flushes;
            if ((c->eip - COMMPAGE_BASE >= COMMPAGE_SIZE || commpage_stub(c->eip)) && !g_flush) {
                target = map_find(c->eip);
                if (!target && often_enough(c->eip)) target = translate(c->eip);
            }
            if (target && !g_flush && l == g_st_flushes && ent >= g_cache_start && ent < g_free)
                *ent = (uint32_t)(uintptr_t)target;
        } else if (c->jit_kind > KIND_INVALID && (c->jit_kind & 1)) {
            /* fill the inline cache of an indirect jump: jump directly to the first target */
            uint32_t *ic = (uint32_t *)(uintptr_t)(c->jit_kind & ~1u), *target = 0;
            unsigned long l = g_st_flushes;
            int cold = 0;
            if ((c->eip - COMMPAGE_BASE >= COMMPAGE_SIZE || commpage_stub(c->eip)) && !g_flush) {
                target = map_find(c->eip);
                if (!target) { if (often_enough(c->eip)) target = translate(c->eip); else cold = 1; }
            }
            /* cold target: the entry stays free (next time it is counted again) */
            if (!cold && !g_flush && l == g_st_flushes && ic >= g_cache_start && ic < g_free) {
                if (target && b_reaches(&ic[4], target)) {
                    ic[1] = ORI(4, 4, c->eip & 0xffff);
                    ic[4] = B_((uint32_t)(uintptr_t)target - (uint32_t)(uintptr_t)&ic[4]);
                    ic[0] = LIS(4, c->eip >> 16);
                } else {                    /* commpage without stub, or a target in the other half: never direct, always via
                                               the table (and the dispatcher) */
                    ic[1] = ORI(4, 4, 0xffff);
                    ic[0] = LIS(4, 0xffff);
                }
                sys_icache_invalidate(ic, 20);
                g_st_ic++;
            }
        } else if (c->jit_kind > KIND_INVALID && !g_flush) {
            uint32_t *stubby_start = (uint32_t *)(uintptr_t)(c->jit_kind - 12), *target;
            unsigned long l = g_st_flushes;
            if (c->var_target) {              /* exit stub of a variant: lis/ori/stw of the target come before it, chaining happens there */
                uint32_t *var_start = stubby_start - 3, *bc = (uint32_t *)(uintptr_t)stubby_start[3];
                target = variant(c->eip, c->var_target);
                if (target && !g_flush && l == g_st_flushes && var_start >= g_cache_start && stubby_start < g_free) {
                    int32_t d;
                    if (b_reaches(var_start, target)) {
                        *var_start = B_((uint32_t)(uintptr_t)target - (uint32_t)(uintptr_t)var_start);
                        sys_icache_invalidate(var_start, 4);
                    } else {
                        far_jump(var_start, target);   /* lis/ori/stw of the target and the stub's first word */
                        g_st_far++;
                    }
                    d = (int32_t)((uint32_t)(uintptr_t)target - (uint32_t)(uintptr_t)bc);
                    if (bc && d >= -32768 && d < 32768) { bc_set(bc, target); sys_icache_invalidate(bc, 4); }
                    g_st_linked++;
                }
                if (!target) c->var_target = 0;     /* no variant: continue via the ordinary block */
            } else if (c->eip - COMMPAGE_BASE >= COMMPAGE_SIZE || commpage_stub(c->eip)) {
                target = map_find(c->eip);
                if (!target && often_enough(c->eip)) target = translate(c->eip);
                /* do not chain if a flush happened in the meantime: the exit stub then belongs to discarded code */
                if (target && !g_flush && l == g_st_flushes && stubby_start >= g_cache_start && stubby_start < g_free)
                    chain_to(stubby_start, target);
            }
        }
    }
}

/* ---------- report ---------- */

typedef struct { uint32_t *code; uint32_t eip; unsigned long hits; } ProfBlock;
typedef struct { uint32_t eip, n; unsigned long hits; uint32_t *code; uint16_t *wt; } ProfEntry;

static ProfEntry *g_pe;
static unsigned g_pe_n, g_pe_max;
static unsigned long g_prof_cache;

static int after_code(const void *x, const void *y)
{
    const ProfBlock *a = x, *b = y;
    return a->code < b->code ? -1 : a->code > b->code;
}

static int after_hits(const void *x, const void *y)
{
    const ProfEntry *a = x, *b = y;
    return a->hits > b->hits ? -1 : a->hits < b->hits;
}

/* collect the hits of the blocks in the cache (before every flush and at the end), with a copy of the code */
static void profile_harvest(void)
{
    ProfBlock *bl;
    unsigned long n = 0, i;
    uint32_t j, k;
    if (!g_nused) return;
    bl = malloc(g_nused * sizeof *bl);
    if (!bl) return;
    for (j = 0; j < g_nused; j++) {
        bl[n].code = g_map[g_inuse[j]].code;
        bl[n].eip = g_map[g_inuse[j]].eip;
        if (bl[n].code) n++;
    }
    qsort(bl, n, sizeof *bl, after_code);
    for (i = 0; i < n; i++) {
        uint32_t *w, *end = i + 1 < n ? bl[i + 1].code : g_free, wordlist = (uint32_t)(end - bl[i].code);
        unsigned long t = 0;
        ProfEntry *pe;
        for (w = bl[i].code; w < end; w++) t += g_prof[w - g_cache];
        if (!t) continue;
        g_prof_cache += t;
        for (k = 0; k < g_pe_n && g_pe[k].eip != bl[i].eip; k++) ;
        if (k == g_pe_n) {
            if (g_pe_n == g_pe_max) {
                g_pe_max = g_pe_max ? 2 * g_pe_max : 256;
                g_pe = realloc(g_pe, g_pe_max * sizeof *g_pe);
            }
            memset(&g_pe[k], 0, sizeof *g_pe);
            g_pe[k].eip = bl[i].eip;
            g_pe_n++;
        }
        pe = &g_pe[k];
        pe->hits += t;
        free(pe->code); free(pe->wt);
        pe->n = wordlist;
        pe->code = malloc(wordlist * 4);
        pe->wt = malloc(wordlist * sizeof *pe->wt);
        if (pe->code) memcpy(pe->code, bl[i].code, wordlist * 4);
        if (pe->wt) memcpy(pe->wt, g_prof + (bl[i].code - g_cache), wordlist * sizeof *pe->wt);
    }
    free(bl);
}

static void profile_report(void)
{
    struct itimerval t;
    unsigned long i;
    uint32_t w;
    FILE *f;
    memset(&t, 0, sizeof t);
    setitimer(ITIMER_PROF, &t, 0);
    if (!g_prof_total) return;
    profile_harvest();
    qsort(g_pe, g_pe_n, sizeof *g_pe, after_hits);
    fprintf(stderr, "profile: %lu samples, %lu in translated code, %lu outside\n", g_prof_total, g_prof_cache,
            g_prof_outside);
    for (i = 0; i < g_pe_n && i < 15; i++)
        fprintf(stderr, "  block %08x  %6lu  %5.1f %%  (%u words)\n", g_pe[i].eip, g_pe[i].hits,
                100.0 * g_pe[i].hits / g_prof_total, g_pe[i].n);
    f = fopen("/tmp/rosetta_blocks_profile.txt", "w");    /* all blocks, for tests/funktionen.sh */
    if (f) {
        for (i = 0; i < g_pe_n; i++) fprintf(f, "%08x %lu\n", g_pe[i].eip, g_pe[i].hits);
        fclose(f);
    }
    f = fopen("/tmp/rosetta_outside.txt", "w");    /* all places outside, for tests/aussen.sh */
    if (f) {
        uint32_t h;
        for (h = 0; h < 4096; h++) if (g_prof_c[h].pc) fprintf(f, "%08x %u\n", g_prof_c[h].pc, g_prof_c[h].n);
        fclose(f);
    }
    for (i = 0; i < 12; i++) {             /* most frequent places outside */
        uint32_t h, best = 0;
        Dl_info di;
        for (h = 0; h < 4096; h++) if (g_prof_c[h].n > g_prof_c[best].n) best = h;
        if (!g_prof_c[best].n) break;
        if (dladdr((void *)(uintptr_t)g_prof_c[best].pc, &di) && di.dli_sname)
            fprintf(stderr, "  outside %08x  %6u  %s+%lu\n", g_prof_c[best].pc, g_prof_c[best].n, di.dli_sname,
                    (unsigned long)(g_prof_c[best].pc - (uint32_t)(uintptr_t)di.dli_saddr));
        else fprintf(stderr, "  outside %08x  %6u\n", g_prof_c[best].pc, g_prof_c[best].n);
        g_prof_c[best].n = 0;
    }
    f = fopen("/tmp/rosetta_profile.s", "w");
    if (f) {
        for (i = 0; i < g_pe_n && i < 8; i++) {
            if (!g_pe[i].code || !g_pe[i].wt) continue;
            fprintf(f, "b_%08x:\n", g_pe[i].eip);
            for (w = 0; w < g_pe[i].n; w++) fprintf(f, "\t.long 0x%08x\n", g_pe[i].code[w]);
            fprintf(stderr, "  hits in %08x per word:", g_pe[i].eip);
            for (w = 0; w < g_pe[i].n; w++) if (g_pe[i].wt[w]) fprintf(stderr, " %x:%u", w * 4, g_pe[i].wt[w]);
            fprintf(stderr, "\n");
        }
        fclose(f);
    }
}
/* sampling profile (main.c): blocks as "host start guest eip" sorted by host address, so hits in the code cache
   can be assigned to a guest address (end = next start) */
static int bl_cmp(const void *a, const void *b)
{
    uint32_t x = ((const uint32_t *)a)[0], y = ((const uint32_t *)b)[0];
    return x < y ? -1 : x > y;
}

static uint32_t *g_bl_t, g_bl_n;

void jit_blocks_sort(void)
{
    uint32_t i;
    free(g_bl_t);
    g_bl_n = 0;
    g_bl_t = malloc(8 * (size_t)(g_nused + 1));
    if (!g_bl_t) return;
    for (i = 0; i < g_nused; i++) {
        uint32_t h = g_inuse[i];
        if (!g_map[h].code || g_map[h].eip == 0xffffffffu) continue;
        g_bl_t[2 * g_bl_n] = (uint32_t)(uintptr_t)g_map[h].code;
        g_bl_t[2 * g_bl_n + 1] = g_map[h].eip;
        g_bl_n++;
    }
    qsort(g_bl_t, g_bl_n, 8, bl_cmp);
}

/* guest eip of the block whose code contains the host address w (0: not in the code cache) */
uint32_t jit_guest_eip(uint32_t w)
{
    uint32_t lo = 0, hi = g_bl_n;
    if (!g_bl_t || !g_bl_n || w < g_bl_t[0] || w >= (uint32_t)(uintptr_t)g_free) return 0;
    while (hi - lo > 1) { uint32_t m = (lo + hi) / 2; if (g_bl_t[2 * m] <= w) lo = m; else hi = m; }
    return g_bl_t[2 * lo + 1];
}

void jit_stats(void)
{
    thunk_stats();
    if (g_st_interrupted)
        fprintf(stderr, "rosetta: interrupted %lu times (%lu block starts and loop heads stopped)\n", g_st_interrupted,
                g_st_ub_starts);
    if (g_cache_loaded) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        fprintf(stderr, "rosetta: translation cache %lu blocks, %lu KB, loaded in %.2f ms (%d page faults), %lu mappings expected, "
                "%lu unbound stubs interpreted, %lu KB held for libraries (%lu KB given to the guest)\n",
                g_st_cache, (unsigned long)((uint8_t *)g_free_loaded - (uint8_t *)g_cache) / 1024,
                (double)g_cache_time * tb.numer / tb.denom / 1e6, g_cache_error, g_st_expected, g_st_inval_cache,
                g_st_hold >> 10, g_st_hold_given >> 10);
    }
    if (g_free >= g_half2)
        fprintf(stderr, "rosetta: code in the second half of the region (%u KB), %lu jumps into the other half, pools %u + %u KB\n",
                (unsigned)(((uint8_t *)g_free - (uint8_t *)g_half2_code) >> 10), g_st_far,
                (unsigned)(((uint8_t *)g_pool[0] - (uint8_t *)g_half_end[0]) >> 10), (unsigned)(((uint8_t *)g_pool[1] - (uint8_t *)g_half_end[1]) >> 10));
    fprintf(stderr,"rosetta: %lu blocks with %lu instructions translated, %lu chained (%lu direct), %lu fused, "
            "%lu page exits, %lu stack slots, %lu floating point (%lu loops, %lu packed), %lu x87, %lu direct (x87/SSE), %lu interpreted in block, %lu interpreted, %lu invalid, %lu flushes, %lu table misses, %lu inline caches, %lu stubs, %lu constants (%lu blocks with guard, %lu failed, %lu masks), %lu loops with prefetch (%lu streams, %lu faults), %lu mov saved, %lu forwarded, %lu calls inlined (%lu predicted, %lu wrong), %lu stack values forwarded (%lu ret without check, %lu dead stores, %lu written later), %lu math calls direct, %lu switch tables, %lu taken branches followed, %lu compares merged across instructions, %lu MMX, cache %lu KB\n", g_st_blocks,
            g_st_insns, g_st_linked, g_st_direct, g_st_fusion, g_st_pages, g_st_slots, g_st_fp, g_st_fp_loops, g_st_pd, g_st_x87, g_st_fp_direct, g_st_inline, g_st_interp, g_st_invalid,
            g_st_flushes, g_st_table_miss, g_st_ic, g_st_stubs, g_st_const, g_st_watchdog, g_st_watchdog_miss, g_st_masks, g_st_prefetch, g_st_vl_streams, g_st_vl_error, g_st_mov, g_st_fw, g_st_embed, g_st_before, g_st_before_miss, g_st_sw, g_st_sw_ret, g_st_dead, g_st_dead_flush, g_st_math_direct, g_st_jumptab, g_st_taken, g_st_fusion_far, g_st_mmx, (unsigned long)((uint8_t *)g_free - (uint8_t *)g_cache) / 1024);
    {
        int i, k, best;
        {
            mach_timebase_info_data_t tb;
            mach_timebase_info(&tb);
            fprintf(stderr, "rosetta: translation took %.1f ms (collecting %.1f, analyses %.1f, generating %.1f, exits %.1f), "
                    "%lu instructions decoded (liveness search %lu); page faults: %d at setup, %d while translating\n",
                    (double)g_trans_time * tb.numer / tb.denom / 1e6,
                    (double)g_time_phase[0] * tb.numer / tb.denom / 1e6, (double)g_time_phase[1] * tb.numer / tb.denom / 1e6,
                    (double)g_time_phase[2] * tb.numer / tb.denom / 1e6, (double)g_time_phase[3] * tb.numer / tb.denom / 1e6,
                    g_decoded, g_st_needed_dec, g_create_error, g_trans_error);
        }
        fprintf(stderr, "rosetta: %lu loops with delayed write-back (%lu checks at entry, %lu failed, %lu back jumps with budget), %lu closed by fall-through, %lu inner jumps, %lu exits with block chaining, %lu calls with continuation, %lu floating point loads hoisted, %lu stubs direct, %lu variants (%lu learned targets), %lu xmm stored in host form, %lu flushes avoided, %lu calls with dormant slots\n",
                g_st_ret, g_st_ret_check, g_st_ret_miss, g_st_budget, g_st_closed, g_st_inner, g_st_chain,
                g_st_cont, g_st_fpahead, g_st_stub_direct, g_st_variants, g_st_var_targets, g_st_xhost,
                g_st_flush_saved, g_st_pauses);
        if (g_threshold > 1)
            fprintf(stderr, "rosetta: threshold %d: %lu cold runs interpreted (%lu instructions), %u addresses counted "
                    "(table 2^%u), prior knowledge %lu blocks\n", g_threshold, g_st_cold, g_st_cold_insn, g_count_used,
                    g_count_bits, g_st_prior);
        fprintf(stderr, "rosetta: interpreted (opcode count):");
        for (k = 0; k < 16; k++) {
            for (best = 0, i = 1; i < 0x400; i++) if (g_interp_op[i] > g_interp_op[best]) best = i;
            if (!g_interp_op[best]) break;
            if (best >= 0x3c0) fprintf(stderr, " %02x/%d:%lu", 0xd8 + ((best - 0x3c0) >> 3), (best - 0x3c0) & 7, g_interp_op[best]);
            else fprintf(stderr, " %s%02x:%lu", best >= 0x300 ? "0f3a." : best >= 0x200 ? "0f38." : best >= 0x100 ? "0f." : "",
                    best & 0xff, g_interp_op[best]);
            g_interp_op[best] = 0;
        }
        fprintf(stderr, "\n");
    }
    {
        int i, k, best;
        unsigned long sum = 0;
        for (i = 0; i < 1024; i++) sum += g_vt[i].n;
        fprintf(stderr, "rosetta: %lu lookups in the map (%lu probes)\n", g_st_find, g_st_samples);
        fprintf(stderr, "rosetta: %lu dispatcher entries, most frequent (eip/reason: count):", sum);
        for (k = 0; k < 10; k++) {
            for (best = 0, i = 1; i < 1024; i++) if (g_vt[i].n > g_vt[best].n) best = i;
            if (!g_vt[best].n) break;
            fprintf(stderr, " %08x/%u:%lu", g_vt[best].eip, g_vt[best].kind, g_vt[best].n);
            g_vt[best].n = 0;
        }
        fprintf(stderr, "\n");
    }
    if (g_prof) profile_report();
}
