# ReverseRosetta: design notes

Goal: run 32-bit Intel programs for Mac OS X 10.5 (command line and graphical) on the iBook G4 (Sorbet Leopard
10.5.9, PowerPC 7447, 1.33 GHz), as fast as possible. Not possible: x86_64, programs that require Snow Leopard or
later.

## Principle

Like Apple's Rosetta, in the other direction: a PowerPC program `rosetta` loads the Intel program into its own address
space, translates the Intel code to PowerPC code at run time and executes it. Everything in the process runs
translated, including dyld, libSystem and the frameworks (in their Intel versions). Only system calls and Mach
messages leave the process; there the data is converted between little endian (Intel) and big endian (kernel,
services).

## Findings (2026-11-13, on the iBook)

- `/usr/lib` still has Intel parts: libSystem, dyld, libobjc, libz, libxml2 etc. (ppc7400 ppc64 i386 x86_64), plus
  `/bin/ls` (i386 ppc7400). Command line programs find everything they need.
- All 163 frameworks are ppc7400/ppc64 only. The Xcode SDK has Intel versions only as stubs (DYLIB_STUB). Real Intel
  frameworks: from Apple's official 10.5.8 Combo Update (universal), stored in a separate root directory; the
  translator redirects file accesses to `/System/Library/Frameworks` there.
- The i386 dyld sits at 0x8fe00000 like the PowerPC dyld, but has 518 local relocations and computes its own slide in
  `__dyld_start`: it is loaded at a different address.
- The i386 libSystem is linked at 0 (built for the shared cache); dyld places it at a free address.
- Address space of a PowerPC process: program from 0x1000, dyld 0x8fe00000, shared libraries 0x90000000-0xb0000000,
  stack 0xbc000000-0xc0000000, commpage 0xffff8000. 0xffff0000-0xffff8000 is free (i386 code expects its commpage
  there).
- CoreGraphics has a Rosetta mode (`sCGIsRosetta`, `gCGShmemSwapRequired`, conversion functions for requests),
  detected via `sysctl.proc_native`. The translator reports "not native" there; the translated Intel CoreGraphics
  then converts to the byte order of the WindowServer itself (to be confirmed).
- Xcode 3.1.4 on the iBook builds i386 programs (`gcc-4.2 -arch i386`), also static without libSystem
  (`-static -nostdlib`, LC_UNIXTHREAD): test programs are built directly on the iBook.

## Speed: the principles

1. **Fixed register assignment:** the 8 Intel registers live permanently in PowerPC registers (r14-r21), plus
   pointers to the state and the translation table. No spilling.
2. **Flags only when needed:** per block, compute backwards which flags are still read; the others are never
   computed. Compare + branch becomes `cmpw`/`cmplw` + `bc` on CR0.
3. **Byte order in hardware:** memory accesses with `lwbrx`/`stwbrx`/`lhbrx`/`sthbrx` (byte reversal at no extra
   cost), guest address = host address (no translation).
4. **Block chaining:** fixed branch targets are linked directly, indirect jumps go through a hash table in the
   generated code, `call`/`ret` through a shadow return stack.
5. **Hot library functions native:** memcpy, memmove, memset, bzero, strlen, strcmp etc. of the Intel libSystem are
   recognized at load time and replaced by PowerPC versions (pointers are the same, no conversion needed).
6. **SSE on AltiVec**, x87 and scalar SSE2 on the FPU (double precision).
7. **Commpage routines** (memcpy, time, locks) native.

## Structure

| Part | File | Task |
|---|---|---|
| Loader | `macho.c` | Fat/thin Mach-O, i386 part, segments at their addresses, LC_UNIXTHREAD, dyld relocated |
| Decoder | `decoder.c` | Intel instruction -> structure (prefixes, ModRM/SIB, operands, length) |
| Interpreter | `interp.c` | Reference and fallback for rare instructions; full flags |
| Translator | `jit.c` | Basic blocks -> PowerPC code, flag liveness, chaining |
| System calls | `sys.c` | BSD calls and Mach traps, convert structures, redirect paths |
| Mach messages | `mach_msg.c` | Convert the header and known messages (kernel, launchd, ...) |
| Commpage | `commpage.c` | Data at 0xffff0000, routines native |

The host `rosetta` itself sits high in the address space (`-seg1addr`), so the guest gets its usual addresses.

## Testing

- Differential testing: interpreter and translator run the same blocks, the state is compared.
- Instruction tests against real Intel hardware: random instruction sequences run natively on the PC (x86) and in the
  translator; registers and flags must match.
- Test programs with checksums, built as i386 on the iBook, expected values from the PC.

## Stages

1. Static i386 program without dyld (write/exit via `int $0x80`).
2. System calls, commpage, i386 dyld + libSystem: `hello` (printf), then `/bin/ls`.
3. Translator (JIT) with all speed principles, measured against the interpreter.
4. Intel frameworks (Combo Update), CoreFoundation/Foundation without a user interface.
5. WindowServer connection (Rosetta mode), first Cocoa window, then real programs.

## Stage 3: translator (status 2026-11-13, evening)

Measured with `tests/measure.sh PROG K G [W]`: instructions per round counted by the interpreter, time per round from
the slope between K and G rounds (startup cost drops out), next to it the same C source as a PowerPC build. Each run
is measured W times (3), the fastest counts. Before, a single run counted: a start of the short run that was 0.1 s
slower shifted the slope by several ms per round; this produced values that were too good (bench_float "108 MIPS", bench_integer
"758", "PIC +5 %"). `tests/compare_env.sh PROG K G VAR=1` measures alternately with and without a switch.

| Benchmark | Interpreter | first JIT | 2026-11-13 late | after the big rework (2026-11-14, night) | native PPC (same C source) |
|---|---|---|---|---|---|
| `bench_integer` (sieve, FNV, insertion sort, Collatz) | 2.4 MIPS | 347 MIPS | ~714 MIPS | **~948 MIPS** | 925 "Intel MIPS" (102 %) |
| `bench_control_flow` (recursion, qsort, switch, strings) | - | 108 MIPS | ~363 MIPS | **~390 MIPS** | 621 (63 %) |
| `bench_float` (scalar SSE2 floating point) | - | 71 MIPS | 125-133 MIPS | **118-125 MIPS** | 219 (54-57 %) |
| `bench_libm` (libm, return via x87) | - | 5 MIPS | ~130 MIPS | **~128 MIPS** | 197 (65 %) |
| `bench_calls` (direct, pointer and table calls) | - | - | ~490 MIPS | **~502 MIPS** | 717 (70 %) |

bench_control_flow by parts (`tests/bench_control_flow_part.c`, `tests/bench_control_flow_parts.sh`, per round): qsort with comparison function 20.9 ms
vs. native 11.6 (55 %), switch machine 15.2 vs. 11.3 (74 %), strings 7.8 vs. 5.4 (69 %); gcc hoists fib(24) out of
the loop (does not count).

**Big rework (2026-11-14, night)**, each with a switch to disable it:
- *Delayed write-back of stack slots* (`ROSETTA_NO_WRITEBACK`): in the loop part of a loop block (up to the last
  backward jump) the slots live only in registers; they are written back before every exit (side exit, exit stub,
  exit, interpreter/C helper in the block) and at the end of the loop part. For this, every other memory access of the
  loop is tracked symbolically (value = entry value of a source + constant; source: register, slot or constant; mov,
  add/sub/inc/dec with immediate, lea, xor r,r). If every source of its address changes by a fixed step per iteration
  (|step| < 4 KB), the access moves monotonically. At the loop entry, one compare per access checks that it lies
  outside the slots; if it moves away from them, nothing more is needed. If it moves towards them (arrays below the
  stack, upwards), the distance limits the iterations: budget = distance / step + 1 in the CTR, the backward jumps
  become `bdnzt`/`bdnzf` (no extra cost on the 7447A, `tools/probes/bdnz.c`); when the budget is used up, the exit
  writes back and re-enters through the block start (`ROSETTA_NO_BUDGET`: only accesses that move away). Across C
  calls in the block, `Cpu.budget` saves the CTR. If the check fails, the block is retranslated with immediate writes
  (`KIND_RET`). Not used when a side exit leads back into the loop part (then every iteration would go through the
  block start: sieve -13 %). Sort loop 22 -> 14 PPC instructions, bench_integer +10 %.
- *Closing loops by fall-through* (`ROSETTA_NO_CLOSE`): if the path falls into its own block start, the block ends
  with a backward jump to the head instead of copying the body a second time (qsort partition loop).
- *Empty prologue*: the alignment nops of a loop head are placed before the block start when nothing is generated
  before the head (otherwise every re-entry ran through them: Collatz alternates between two blocks). bench_integer +5 %.
- *Inner jumps* (`ROSETTA_NO_INNER`): a conditional jump to a later instruction of the same block (if/else,
  `continue`, the `jl` path of an embedded comparison function) jumps directly into that instruction's code instead
  of leaving the block. At the merge point: floating point and x87 in memory, flags (if live) in memory, no fused
  compares/movs across the boundary, floating point forwarding empty, stack forwarding = intersection (entries with a
  sequence number, esp/ebp only at equal state). No calls in between; not from the loop part with delayed write-back,
  not in blocks with floating point loop registers or prefetch. bench_integer +13 % (sieve and Collatz stay in the block,
  102 % of native), qsort +12 %, switch machine +17 %. Backward inner jumps (inner loops in the block) are opt-in only
  (`ROSETTA_INNER_RET=1`): slower on the G4 (bench_integer -7 %), because separate blocks have aligned heads and separate
  branch copies are predicted better by the 2-bit predictor per address (Collatz: "odd is followed by even").
- Bug fixed: instructions interpreted in the block (also the fallback of div) and `fp_direct` did not reload the
  stack slots afterwards (a slot written there stayed stale in the register).
- *Jump threading* (`ROSETTA_NO_CHAIN`): if a side exit with flags in registers jumps to a conditional jump (qsort:
  `cmp eax,0; jge` -> `jne`), the exit stub decides that jump right away and goes directly to its target. qsort +10 %.
- *4-way inline cache* (`ROSETTA_IC_WAYS`, 1-8): each new target of an indirect jump/call gets its own compare with a
  direct `b`, only then the hash table. switch machine +8-12 %.
- *String stubs called directly* (`ROSETTA_NO_STUB_DIRECT`): strcpy/strtok/strlen ... are called directly at the call
  site like the libm (return address on the stack, C wrapper, eax back), without stub block, shadow stack and `ret`;
  the block continues after the call. String part of bench_control_flow 63 -> 78 %.
- *Variants of indirect calls* (`ROSETTA_NO_VARIANTS`): if the target prediction of a `call *` fails, the call site
  learns the target (up to 4, `g_targets`) and the block is retranslated: the predicted target stays embedded, the
  exit compares the others and jumps into a *variant* - a block from the call site for exactly this target (embedded,
  without guard; own table `g_var`, exit stub with `Cpu.var_target`). In variants, a jump back to the call site does
  not go to the variant's own head (`keip`). From the fifth target on: generic call. In addition, `precompute()` also
  predicts the target when the block computes the address registers itself beforehand (concretely, from the entry
  state; only in loops, otherwise it cost 4 % startup time). Table calls with 4 alternating targets 44 -> 67 % of
  native, bench_calls 74 -> 80 %.
- *Prologue only from constants*: padding for the aligned head before the block start (entry without nops).
- No gain (opt-in only): continuation after calls in the block (`ROSETTA_CONT=1` in loops, `=2` everywhere: bench_control_flow
  +2 %, but 38 % more translated instructions, startup +17 %), hoisting floating point loads (`ROSETTA_FPAHEAD=1`: the
  7447A overlaps independent load sequences itself).
- **Measurement trap**: while the battery charges (a charge-limit program holds it at 80 %), the 7447A runs at about
  half clock; until 2026-11-14 02:10 everything ran throttled (start of `hello` ~0.2 s instead of ~0.11 s). Ratios to
  native are correct, absolute MIPS are not; a change in the middle of a measurement series produces nonsense. Check
  `tests/startup.sh` before measurement series.

Status 2026-11-14 at full clock (15 comparisons equal): bench_integer 1873-1916 MIPS (101-103 % of native), bench_control_flow 863-932
(68-77 %), bench_float 238-245 (55-57 %), bench_libm 260-268 (65-67 %), bench_calls 1154 (80 %), start of hello 0.118 s.

After that (2026-11-14, night):
- *xmm in host form at block boundaries* (`ROSETTA_NO_XHOST`): exits store the low value of a modified xmm register
  with stfd/stfs in `Cpu.xmm_h` and set `Cpu.xmm_hf[n]` ('D'/'F'), instead of byte-swapping it into Intel format with
  ~10 instructions (stfd, lwz, lwz with stall, stwbrx ...); loading checks the tag byte (lfd/lfs, otherwise the
  AltiVec path). The Intel format is produced only when needed: in the block in `xf_back`/`xf_forget` (for registers
  that are not cached, by a run-time check, `xmm_mat_code`), in C code by `xmm_intel()` at the start of `step()` and
  `fp_direct()`. n-body +10 %.
- *Floating point forwarding with stack knowledge* (`ROSETTA_NO_FW_STACK`): a store to `[esp+d]` or a push discards
  entries for `[ebp+d]` only when they overlap according to the symbolic esp/ebp. bench_libm +2-3 %.
- *Address `[base+d]` for floating point accesses* with one `addi` (no instruction for d = 0) instead of `li`+`add`.
- *Page-exact flush* (`ROSETTA_NO_PAGEEXACT`): mprotect/munmap/mmap(MAP_FIXED) discard the cache only when they hit a
  page that code was translated from (bitmap `g_codepages`, also pages of predicted constants). At the start of hello
  all 14 flushes disappear, 1562 instead of 2778 blocks: **startup 0.122 -> 0.080 s**. Fixed along the way: after a
  flush the return stack still pointed into discarded code; it now starts again at the beginning of the ring (the
  sentinel entry before it sends old `ret`s through the table).
- No gain: setcc via mfcr after compares with memory data (`ROSETTA_SETCC_CR=1`), switch tables again (-15 %), block
  size 64/96 instead of 48.

Status after these changes (full clock): bench_integer 1904 (103 %), bench_control_flow 897 (71 %), bench_float 259 (59 %), bench_libm 278
(70 %), bench_calls 1193 (83 %), start of hello 0.080 s. Parts: matrix 31 %, n-body 31 %, float vectors ~30 %, division
105 %; qsort 64 %, switch 81 %, strings 79 %; table calls 67 %.

After that:
- *Exact store size in the forwarding* (`ROSETTA_NO_EXACT_SZ`): every store discarded 16 bytes in the stack and the
  floating point forwarding as a precaution; `mov [esp],eax` thus deleted the entry recorded immediately before for
  `[esp+4]` - the second argument of every embedded call came from memory. Now the size comes from `mem_access()` (x87 still 16,
  because of m80). bench_calls +7 %, qsort +2-4 %.
- No gain (opt-in only): stack slots that rest during embedded calls (`ROSETTA_PAUSE=1`, with a check at the entry
  that argument and stack frame writes lie below the slots): qsort at most +3.6 %, bench_calls -8 % - the slots take the
  registers r28-r31 away from the stack forwarding, which needs them for argument and ebp copies.
- Translation measured (`tests/translate_time.sh`, counters with ROSETTA_STATS): ~60 of ~80 ms at the start of hello,
  ~2 decodes per translated instruction; a page fault costs ~10.5 us on the iBook (`tools/probes/page_faults.c`),
  hello has 721 against 86 native.

Status after these changes: bench_integer 1846-1904 (100-103 %), bench_control_flow 913-929 (73-74 %), bench_float 253-262 (58-60 %), bench_libm
261-278 (66-70 %), bench_calls 1187-1195 (83-84 %), start of hello 0.08 s.

**Translation time (2026-11-14, night).** Of ~86 ms startup (hello), ~70 ms go to translating 1571 blocks with 22838
instructions, i.e. ~3 us (~4000 cycles) per Intel instruction. Breakdown (`ROSETTA_STATS`): collecting 14, analyses
15, generating 30, exits 5 ms. 865 page faults (~9 ms) in total. Tools:
- `ROSETTA_HPROFILE=file` (main.c): samples every 100 us via SIGALRM/ITIMER_REAL (ITIMER_PROF only has a resolution
  of 10 ms), per sample srr0, lr and the frame chain. `tests/profile.sh PROG N` shows self/inclusive per function,
  `tests/line_profile.sh PROG N` builds `build/rosetta_g` (-g, same optimization) and maps the hits to source lines
  with gdb.
- The profile is flat (many functions with 1-3 %); largest items: slots_choose 10 % inclusive, decode 9 %, gen 8 %,
  write_mask 6 %, embed_worth 4 %, needed 3.5 %, page_prot 3 %, thunk_find 3 %.
- Countermeasures: write mask cached in the instruction, x87_can checks the opcode range inline, slots_choose stops
  immediately when there is no loop, needed and embed_worth remember their result per target (cleared with the cache
  and on every mmap/mprotect of the guest), page_prot and thunk_find first check the most recently hit range,
  libSystem symbol names are prefiltered by first letter and length, the first flush skips the memsets.
- *Tiered translation* (`ROSETTA_THRESHOLD=n`): a block is translated only when the dispatcher enters it for the n-th
  time; until then `cold_run` interprets up to the next jump (a ret to a translated call removes the shadow entry).
  Exit stubs are chained, and inline cache entries filled, only when the target is hot.

**Startup, 2026-11-14 morning** (measured with `tests/ab.sh` alternating, `tests/tiers.sh`, `tests/base_cost.sh`):
- Tiered translation is now the default, threshold 10 (hello fastest at 15, ls at 8; +/-1 % in between). Bug found in
  the process: a function with a native version (strtok), interpreted cold, ran in the Intel version with its own
  state, and the later translated calls ran in the host - bench_control_flow computed wrong results. Such addresses count as hot
  immediately (`often_enough` looks each address up once in `thunk_find`).
- Cold interpretation queried the map after every instruction (a cache miss ~100 ns, 7 % of startup): removed.
- Interpreter: `step` has twelve saved registers and cr3/cr4; prologue and epilogue cost 17 % per instruction. Now the
  body is `step_core` (always_inline), `steps()` runs up to the next jump in a loop, the decoder (`decoder_core.h`) is
  inlined. 6.7 -> 8.0 million instructions/s.
- Build options `-O3 -mdynamic-no-pic` (without PIC, the `bcl`/`mflr` in every function that uses globals
  disappears): startup -6 %, `-O3` another -2 %, sustained performance unchanged. `-Os` brought nothing.
- Stubs: import stubs are resolved only when the translator sees a call to them; the stub addresses of the libSystem
  are stored per UUID in `~/Library/Caches/rosetta/stubs-UUID` (binary file with a checksum of the stub list,
  `ROSETTA_NO_STORE=1`): saves reading `__LINKEDIT` (14 page faults, ~0.5 ms).
- Rejected (measured): a cache for decoded instructions (2048 entries) - on the G4 every access into the 96 KB table
  costs as much as the decoding itself, startup +2 ms. Binary search of the stub names - slower than one linear pass.
- Time floor: every process start via `system()` costs 12.2 ms (also native). hello under rosetta 44-46 ms (before:
  84 ms), of which translating ~37 %, interpreting ~22 %, system calls ~12 %.
- Page faults (~10 us each; `ROSETTA_STATS` shows them by phase): 723 -> 577. An empty program already has 159 up to
  main, rosetta 225. The first flush skips the memsets of `g_cpool_h` and `g_vl_pc` (48 faults); the jump table
  (512 KB) is only filled when an indirect jump misses it (before, every new block touched a different one of its 128
  pages; occupied entries are kept in `g_tab_slot` for flushing); the map starts with 2^10 instead of 2^14 entries
  and grows. hello 42.5 -> 39.6 ms, ls -la 146.7 -> 142.8 ms.
- System calls (`ROSETTA_STATS` shows time per number, `ROSETTA_LOG` the sysctl names): ~3.8 ms, almost all of it
  kernel work for the guest dyld (mmap of the 1.4 MB libSystem ~0.5 ms, sysctl ~55 us, open ~60 us). The type of a
  sysctl OID is remembered (second call per sysctl); the effect is barely measurable.
- Rejected (measured): two tiers - blocks without a loop first translated without embedding and stack forwarding,
  with a counter at the block start, fully translated after 200 entries. hello +0.9 ms, ls +21 ms: ls has many hot
  blocks without a loop (433 of 793 were translated twice), and code without embedding runs noticeably slower.
- Threshold now 12 (hello 38.4 ms, ls -la 141.6 ms).
- The counter table of tiered translation grows (open hash table, from 2^12, x4 at half load) instead of evicting
  after 8 probes: with 2048 fixed entries, blocks of ls stayed interpreted forever (145 -> 237 ms); with large
  programs (GUI, tens of thousands of addresses) this would have happened even with 8192.

**Signals to the guest** (2026-11-14 morning, sys.c section "signals to the guest"): sigaction records the handler,
the trampoline (`_sigtramp` of the libc from `struct __sigaction`), mask and flags, and sets up the host accordingly
(handler -> host handler that only records the signal; SIG_IGN/SIG_DFL as for the guest, so SIGPIPE ignored ->
EPIPE). The old action has the layout of `struct sigaction` (12 bytes; before, 16 bytes including the trampoline were
written - 4 bytes past the target). kill/`__pthread_kill` to the process itself record the signal, sigprocmask
maintains `g_sig_mask` (bit n-1 for signal n, like `sigmask()`), sigsuspend waits without losing signals (host
signals blocked until the wait), setitimer/getitimer go to the host timer. Delivery happens at the dispatcher (and in
`run`): stack frame like xnu `sendsig` for 32 bit - mcontext32 (registers, eflags, floating point with xmm, mxcsr,
fcw; the x87 stack as doubles with a tag for sigreturn), siginfo, ucontext32 (the previous mask, or the one from
sigsuspend), sigframe32 (retaddr -1, handler, UC_TRAD/UC_FLAVOR, sig, sinfo, uctx; esp 16-byte aligned after the
return address word), eip = trampoline. sigreturn (184) reads the state back (including changes made by the handler),
`sys_sysenter` then leaves eip/esp alone. SIG_DFL terminates the host with the same signal (exit status to the shell
as native), SA_RESETHAND and SA_NODEFER work. Tests `signals` (raise, SA_SIGINFO, block/unblock, alarm + pause,
ualarm + usleep, SA_RESETHAND, SIGPIPE) and `signal_exit` (SIGTERM without handler). Found along the way:
`__semwait_signal_nocancel` (334) returned ETIMEDOUT immediately - usleep/nanosleep never slept; 333/334 now go to
the host.
Limit: a loop in translated code that neither makes a system call nor leaves the block does not pass the dispatcher -
a signal then waits until it does. A fix without cost in hot code would be to redirect all chainings and backward
jumps to exits in the host handler (needs a list of all patch sites).
Synchronous faults (SIGSEGV/SIGBUS/SIGILL/SIGFPE in the guest) are not yet delivered to guest handlers (division
errors, ud2 and int3 are since 2026-11-15, see "Robustness").

**Stage 4: Intel frameworks** (from 2026-11-14, late morning). Source: Apple's Combo Update 10.5.8
(support.apple.com/106460, updates.cdn-apple.com, 797 MB; package signed by "Software Update, Apple Inc." via Apple
Root CA). Only unpacked, not installed: `intel/download` (disk image), `intel/packages` (pkgutil --expand),
`intel/raw` (payload, 2.3 GB), `intel/root` (`tools/intel_root.sh`: every Mach-O file with an i386 part as a thin
i386 version under the same path, plus the symbolic links; `tools/intel_prune.sh` removes what the system itself has
with i386). The update contains 68 frameworks with i386 (CoreFoundation, Foundation, AppKit, IOKit, Security,
SystemConfiguration, QuartzCore, OpenGL ...). rosetta redirects the guest's `open`, `access`, `stat`/`lstat`(64)
under /System/Library, /usr/lib and /Library to `$ROSETTA_INTEL` (default `~/tools/rosetta/intel/root`) when a file
exists there; directories and resources come from the system. i386 programs are linked against frameworks with
`-isysroot /Developer/SDKs/MacOSX10.5.sdk` (stub frameworks with i386). The tests `tests/cf.c` (CoreFoundation),
`tests/objc_plain.m` (Objective-C without Foundation, with a category) and `tests/foundation.m` (Foundation, own
class, exceptions) produce byte-identical output to native (run_tests.sh: 22 comparisons).
- Umbrella shells (CoreServices, ApplicationServices, Carbon, Cocoa) are not in the update: `tools/intel_umbrellas.sh`
  builds them as empty i386 libraries with the ID/versions of the SDK stub and `-reexport_library` for every
  subframework that exists as i386 (missing: DictionaryServices, 7 Carbon parts).
- Newly required: sockets (socket, connect, bind, listen, accept, getsockname/getpeername, socketpair,
  set/getsockopt, sendto, recvfrom, sendmsg/recvmsg with SCM_RIGHTS), select (fd_set words), poll,
  pathconf/fpathconf. NSLog talks to syslogd over a Unix socket.
- **Objective-C categories**: the i386 libobjc reads them in `_objc_read_categories_from_image` only if
  `sysctl {CTL_KERN, 45}` (KERN_EXEC/KERN_CLASSICHANDLER) returns a non-empty string - on an Intel Mac the path of
  Apple's Rosetta (`/usr/libexec/oah/translate`), empty on the PowerPC kernel. Without categories, Foundation lacked
  most methods (`-[NSCFString UTF8String]: unrecognized selector`). rosetta now answers like an Intel Mac. Found with
  `ROSETTA_HALT=addr,...` (interpreter only: registers and stack at given addresses) and `tools/disasm_lib.sh`,
  `tools/nm_names.sh`.
- Times (fastest of 3, `tests/time_vs_native.sh`): CoreFoundation test native 18 / rosetta 126 ms, Foundation 46 /
  205 ms, Objective-C 14 / 72 ms. Natively the frameworks come prebound from the shared cache; under rosetta the guest
  dyld loads and binds every file itself.
- **Guest profile** (`tests/guest_profile.sh PROG N`, `guest_profile_only.sh`): for every sample, the sampling
  profiler writes the block in the code cache and the guest eip as image+offset to `<file>.guest` (images: i386 Mach
  headers in guest memory below 0x90000000, read with vm_read); the script resolves them through the i386 symbols.
  `tests/images.sh PROG` (`ROSETTA_IMAGES=1`) shows the actual and the preferred address of every image.
- Finding: Leopard's i386 libraries are linked at address 0; loaded from disk, each of them is rebased by dyld
  (`doRebase` 10 %). `tools/intel_rebase.sh` fixes all 334 libraries of the root at addresses from 0x40000000 upwards
  with Apple's `rebase` (plus thin i386 copies of the system libraries from /usr/lib); now every image loads at its
  preferred address. Small gain (Foundation 197 -> 193 ms, page faults 2562 -> 2163): the copy-on-write page faults
  move to binding. Also: dyld has its own strcmp/strlen/memcmp/... (local symbols, cdecl) - now the same stubs as for
  the libSystem (strcmp was 2 %).
- Status at midday: Foundation 193 ms (native 47), CoreFoundation 119 (18), Objective-C 69 (14), hello 40 (13). The
  rest of framework startup: system calls ~14 % (mmap/open/stat per image), interpreting ~25 %, translating ~30 %,
  binding. The biggest remaining lever would be a translation cache across runs.
- **Prior knowledge** (`jit_prog`, `hot_load`, `jit_end`): almost all interpreting at startup was the warm-up of
  blocks that are translated later anyway (threshold-1 executions per block). At the end of a run the list of
  translated blocks (merged with the old one, written only when something is new) goes to
  `~/Library/Caches/rosetta/hot-<program>-<build>`; at the next start their counters are set to full right away.
  Wrong entries only cost a translation. `ROSETTA_NO_PRIOR=1`. Foundation 194 -> 165 ms (interpreted 224000 -> 62000
  instructions), CoreFoundation 119 -> 101, ls -la 142 -> 112, hello 40 -> 38. With the list the threshold hardly
  matters (10 stays for first runs). Intel perl now runs too (libperl from the update).
- **Translation cache** (2026-11-14 afternoon; `cache_write`, `cache_load`, `jit_mapping_expected`): at the end of a
  run the whole translated code, with everything it depends on (map, code pages, constant pool, prefetch sites,
  variants, jump table), goes to `~/Library/Caches/rosetta/cache-<program>-<build>-<environment>`. This works because
  all addresses are fixed: code cache at 0x30000000 (code from the next page boundary, prologue of fixed length,
  g_table via lis/addi), rosetta without PIC, guest libraries rebased. The key contains all `ROSETTA_*` variables
  (except profile/log/display) and addresses in the host libSystem (math functions are called directly). At startup
  the code is mapped with `mmap(MAP_PRIVATE)` (no copying, no zero pages; the kernel syncs the caches when it first
  maps a page): 1.3 MB in 2 ms. Program and dyld are checked immediately, each library when dyld maps it to its place
  with mmap (fstat: size, time, inode number). In that case nothing is flushed; any other mapping over pages with
  code flushes as always before code from it runs. The cache is saved only when there was no flush, only for code
  from files and the commpage (otherwise not at all), and only when new entry points were added. With the cache the
  threshold is 1 (cold code costs only once; `ROSETTA_CACHE_THRESHOLD`). Unbound dyld stubs (`call` into the binder
  instead of `jmp`) make cached blocks fail the byte check once: then the call is interpreted and the block kept
  (otherwise two translations and copied pages per stub). Switches: `ROSETTA_NO_CACHE=1`, `ROSETTA_CACHE_SHOW=1`
  (path, reason on rejection) or `=2` (every new block).
- **Read-only file mappings shared** (sys.c): under Leopard, `MAP_PRIVATE` of a file whose pages are resident costs
  time per resident page of the whole file (protection for later copy-on-write): Foundation's `__TEXT` 200 us,
  `MAP_SHARED` 5 us (`tools/probes/mmap_cost.c`). Without write permission the guest sees no difference; if it asks
  for it later (mprotect, vm_protect = Mach message 3803), the range is first replaced by private memory with the same
  content. `ROSETTA_NO_SHARED=1`. Rejected: writable private mappings as anonymous memory + pread
  (`tools/probes/data_map_cost.c`: faster in isolation, 1 ms slower measured in a real run).
- **Own prebinding** (`tools/prebind.c`, `intel_prebind.sh`): Apple's i386 libraries are not built prebound
  (`redo_prebinding`: "file is not prebound"). dyld skips binding an image if it carries MH_PREBOUND, sits at its
  address and the timestamps of its dependencies match; then `setupLazyPointerHandler` does not reset the jump tables
  either. The prebinder resolves like dyld (two-level, re-exports via LC_REEXPORT_DYLIB, subframeworks and
  sublibraries) and writes external relocations, non-lazy pointers and jump tables (`jmp`), sets n_value of the
  undefined symbols to the address (dyld computes the addend back from it if the prebinding does not match) and leaves
  lazy pointers untouched (without PB_LA_PTR relocations dyld could not reset them). 266 of 337 libraries (the rest
  depend on frameworks without an i386 version or use flat lookup). dyld with Foundation: 26 of 27 images without
  fixups, binding 14.3 -> 1.8 ms, lazily bound 239 -> 0. **Run it again after every `intel_rebase.sh`.** Backup:
  before its first run, `intel_prebind.sh` saves the whole root as a tar archive in `intel/`.
- **Atomic commpage routines as stubs** (CAS 32/64, xadd, bts/btr, spin_lock*, barriers, gettimeofday): they ran
  instruction by instruction through dispatcher and interpreter (OSAtomicCompareAndSwap32 ~1000 times per Foundation
  start). Register stubs: the block stores all guest registers before and reloads them after; flags lazy as for
  cmp/add, or CF. Without threads, "atomic" is a plain read-modify-write (to be made truly atomic again for threads).
  Dispatcher entries 3700 -> 550. Also `cld`/`std` translated, the map sized correctly at load time, the path
  redirection caches its answers.
- The cache records which file an image is when it is mapped (fstat on dyld's descriptor), not through the install
  name: libperl is named `/System/Library/Perl/lib/5.8/libperl.dylib`, but what is loaded is
  `.../5.8.8/darwin-thread-multi-2level/CORE/libperl.dylib` - through the name, the system's PPC libperl ended up in
  the cache, every run flushed and never wrote a new one. A cache that still triggers a flush during a run is deleted
  (the next run creates it again). perl -e: 130 -> 34 ms (native 18).
- **`cmpsb` translated** (`gen_cmpsb`, also repz/repnz): gcc -Os inlines strcmp with literals as `cld; repz cmpsb`,
  dyld compares every segment and section name this way (hottest single block at Foundation startup, an interpreter
  call in the block every time). Now a PPC loop on ecx/esi/edi, step from DF, flags like `cmp byte` into memory
  (ecx = 0 leaves the old ones). Test: `tests/cmpsb.c` against the interpreter (test suite: 23 comparisons).
  `ROSETTA_NO_CMPSB=1`. Foundation -1.3 ms. Rejected: a folded index for the jump table (after the commpage stubs
  only ~10 misses per run), copying small private segments (no difference in the alternating comparison).
- Caution when comparing: `objc_plain` needs only libobjc; the generic `.m` rule in build.sh also linked Foundation
  (native then 44 instead of 14 ms). Now it has its own rule.
- Status (2026-11-14 early afternoon; steady state, fastest of 5): Foundation 68 ms (native 46, earlier that day 163),
  CoreFoundation 40 (18; 101), Objective-C 28 (14; 61), hello 22 (13; 38), ls -la 64 (42; 107). Without the cache:
  Foundation 138 ms. The rest for Foundation: guest code ~53 % (objc runtime at startup ~15 %: uniquing selectors,
  class tables), system calls ~25 % (mmap of the private segments, mach_msg/vm_allocate, stat/open/pread per library:
  natively everything comes from the shared cache), setup including the cache ~3 %. Measurement tools:
  `tests/cache_startup.sh`, `run_all.sh`, `guest_profile.sh`, `dyld_stat.sh`, plus a host profile by library
  (dladdr).

**Threads** (2026-11-14 afternoon, `thread.c`): pthread_create of the i386 libSystem calls bsdthread_create (360);
every guest thread gets its own host thread with its own Cpu (page-aligned) and its own return ring. rosetta sets up
stack, guard page and pthread structure like xnu, and starts the thread in the `_thread_start` reported at
bsdthread_register (366) with eax = pthread_t, ebx = port (a separate send right reference), ecx = function, edx =
argument, edi = stack size, esi = flags, esp = stack end - 16. bsdthread_terminate (361) frees the stack, signals the
join semaphore and ends only this thread; exit from any thread ends the process (cache, statistics).
- **One global lock**: guest code always runs in only one thread at a time. On the iBook (one core) this costs no
  parallelism; in return, lock/xchg/cmpxchg and the commpage stubs stay atomic, and translator, map, code cache and
  flushing need no locks of their own (a flush resets the rings of all threads; the others wait outside translated
  code). The lock is released only outside translated code (system calls always end a block): blocking calls
  (read/write/readv/writev, select, poll, wait4, sockets, __semwait_signal, mach_msg, semaphore waits,
  swtch/thread_switch, mach_wait_until) through wrappers that already have their arguments and restore g_arg_base
  afterwards; at the dispatcher after 2 ms when another thread is waiting; on a held spin_lock (the stub goes to the
  dispatcher and retries) and on `pause` (now ends the block). mach_msg buffer per thread (`Cpu.msg`), sigsuspend with
  threads by polling (the host delivers signals to any thread).
- **Kernel work queues** (workq_open 367, workq_ops 368): NSOperationQueue under 10.5 uses them; without them
  Foundation passed a null pointer to pthread_workqueue_additem_np. Rebuilt after xnu-1228: QUEUE_ADD queues the item
  and wakes an idle worker thread or creates a new one (up to 8), which runs in `_pthread_wqthread` (eax = pthread_t
  at the stack end, ebx = port, ecx = stack start, edx = item, edi = reused); THREAD_RETURN fetches the next item or
  idles (lock released). Also Mach trap 35 (semaphore_signal_thread, pthread_cond_signal), BSD 331
  (__disable_threadsignal), 142 (gethostuuid).
- Tests: `tests/threads.c` (mutex, condition, OSSpinLock, atomics, keys, pipe, detach, many short threads, exit with a
  running thread) and `tests/threads_ns.m` (NSThread, NSLock, NSCondition, NSOperationQueue, wait loop with pause)
  against native; translated, threshold 1 and interpreter give the same results (test suite: 25 comparisons).
  Single-threaded programs: startup and benchmark figures unchanged. Foundation thread test in steady state 132 ms
  (native 79), the C thread test often faster than native (fewer switches around contended locks). With threads
  every run interleaves differently: the cache is written only when there is a significant amount of new code (32
  blocks and 1.5 %).
- `ROSETTA_STATS` shows the wait time and the idle time of the lock.

**Interruptible compute loops** (2026-11-14 evening): a loop in translated code does not pass the dispatcher - a
waiting thread never got its turn, a signal (also Ctrl+C with a handler) was never delivered. Without cost in normal
operation: a watchdog thread (thread.c, starts with the guest's first thread or signal handler) checks every 4 ms; if
someone is waiting (a thread for the lock, an unblocked signal), and the thread is in translated code and has not
been in the dispatcher for two rounds, the watchdog halts it (`jit_interrupt`): the first instruction of every block
start becomes `bl interrupt` (trampoline in the frame code: mflr r3, r3 -= 4, kind 16, exit; the dispatcher finds
the guest eip in the list of changes), every loop head behind a prologue becomes a jump to its own exit (generated at
the end of the block, never executed otherwise: writes back delayed stack slots and preloaded floating point values,
then exits with eip = block start). Every loop enters a block start or its head. At a block start the guest state is
complete (contract of chaining), LR is not needed there. The dispatcher restores everything (in reverse order) before
it does anything else; a flush forgets the head list, the translation cache stores it as well (and is written
unpatched). Race with the translator: Dekker pair `g_in_code` / `g_patch_wants` (one core; the watchdog writes only
while the thread is in translated code, the dispatcher waits after the exit until the watchdog is done).
`ROSETTA_NO_WATCHDOG=1`.
- Rejected: a fixed CTR budget for loops within a block (bdnz instead of b should cost nothing): bench_integer -19 %, because
  the Collatz loop executed li/mtctr and alignment nops at every re-entry; only for loops with a prologue: bench_calls/bench_control_flow
  still -3 to -6 % (bdnzf in the hot loop is more expensive than bne on the 7447A).
- Tests: `tests/interrupt.c` (SIGALRM in a tight, a branching and a calling loop; a thread waits for another one
  without pause), `tests/ctrl_c.sh` (SIGINT from outside, own handler, endless loop: reaction as native); without the
  watchdog both hang. Test suite: 27 comparisons. Benchmark figures unchanged (bench_integer 103 %, bench_control_flow 72-73 %, bench_float
  59 %, bench_libm 69 %, bench_calls 81-83 %).
- Finding along the way: with the cache, bench_control_flow varies between 910 and 1037 MIPS with the same cache file - placement
  of the pages in the physically indexed L2, no difference in the code.

**Packed SSE2 doubles** (2026-11-14 evening, `gen_pd`): until then every `addpd`/`mulpd`/`movapd`... went through the
direct call into C (bench_sse2: 26 MIPS, 10 % of native). Now native: the low double in f14+n as for scalar SSE, the high
one in f24+n (`XfState.h/hs/hhome`: in the FPR, memory stale, still waiting in the storage slot). f24-f31 otherwise
belong to the x87 stack; blocks with x87 instructions (`g_xfh_on = 0`) take the path through Cpu.xmm for the high
halves. `high` (the high half in memory still has to become 0) and `h` are mutually exclusive. A 16-byte memory
operand is byte-swapped once with AltiVec (pattern `Cpu.vswapp`: each double separately) and placed in a storage
slot; both halves are loaded with lfd, only when used (`movapd` load: both wait on the same slot; copy without the
FPU; `movapd` store from the slot, swapped back). Constant operands (PIC) come from the constant pool. Translated:
add/sub/mul/div/min/max/sqrt pd, minsd/maxsd, movapd/movupd, movlpd/movhpd (also movlps/movhps, movhlps/movlhps),
unpcklpd/unpckhpd, shufpd, cmppd/cmpsd (fcmpu, mask via mfcr into Cpu.xmm), movmskpd, and/andn/or/xor pd (bitwise on
the swapped form in the storage slot, `xorpd x,x` as 0), cvtdq2pd, cvt(t)pd2dq, cvtps2pd, cvtpd2ps, movq (like movsd,
high half 0). min/max as defined by Intel (a < b ? a : b, otherwise b: NaN and equal zeros). sqrtpd: libm twice. Loop
registers also keep their high half across the backward jump (`g_fp_ph`).
- bench_sse2 (`tests/bench_sse2.c`: daxpy, dot product, complex multiplication, normalization, compare with mask,
  conversion): 26 -> 180-207 MIPS (65-75 % of native, varies a lot: short rounds). Without high halves in the FPR
  39 %. Real library: Intel's libvDSP/libvMisc (vDSP, FFT, vForce) translated 3.6x faster (100 rounds 2.60 ->
  0.72 s).
- Hoisting the swapped operands (`ROSETTA_FPAHEAD=1`, now also for 16 bytes) brings nothing measurable, stays off.
- Tests: `tests/pd_ops.c` (all instructions with NaN, -0, infinity, denormals, overflow on conversion, unaligned, plus
  x87 fistp in all rounding modes) against the interpreter, `bench_sse2` against native, `vdsp_d.c` against "interpreted
  with the same native math functions" (the pure interpreter computes sin/cos with Intel's code: FFT twiddle factors
  differ in the last bit, as intended). Test suite: 32 comparisons. Switches `ROSETTA_NO_PD`, `ROSETTA_NO_XFH`.
- Old bugs found along the way: (1) cvtsd2si/cvtpd2dq and x87 fistp (round to nearest, or round up) checked overflow
  at the source against 2^31; but 2147483647.6 rounds to 2^31 and must give 0x80000000. Now the rounded value is
  compared ((x + 2^52 + 2^31) - (2^52 + 2^31), or the result of x87_round). (2) Constant prediction computed straight
  through jump targets inside the block: a register that got a different `lea` on one path was predicted wrongly
  (vvsin). At such targets, only what the whole block does not change is valid now. Hidden until now because
  movmskpd before it went through C and discarded all knowledge.
- Debugging: `ROSETTA_CONST=lo-hi` (prediction only in these blocks, `tests/bisect_const.sh`), `ROSETTA_ONLY=lo-hi`
  (translate only these blocks, `tests/bisect_blocks.sh`).

**Real Intel programs** (`tests/intel_tools.sh`: universal binaries from /bin and /usr/bin against native): cat, wc,
sort, grep, awk, sed, find, bc give byte-identical output (1.7-3.7x native, short runs dominated by startup). perl
and tclsh need libperl and Tcl.framework, which exist here only for PowerPC (stage 4; perl now runs with libperl from
the update, tclsh does not: the Combo Update contains no i386 Tcl.framework). Found along the way:
- memcpy/bzero/bcopy of the commpage (the libSystem jumps into them with `jmp *eax`) went through table, dispatcher
  and `step()` every time - sed: 60000 times. Now stub blocks (`commpage_stub`, `ROSETTA_NO_CP_STUB=1`): sed 142 ->
  123 ms.
- 16-bit arithmetic instructions (66 prefix: `orw`/`andw`/`cmpw` on `FILE._flags` of stdio) were interpreted in the
  block (27000 times in sed). Now translated (classify, fusion, mem_access, write_mask and the code generators use
  `opsz`; the symbolic stack analysis does not treat 16-bit add/sub as pointer arithmetic); a stack slot is also
  excluded when it is read with 8/16 bits (with delayed write-back, memory would still hold the old value). sed ->
  115 ms. Tests `word16` (16 such instructions, flags, signed and unsigned compares) and `sed (Intel)` in
  run_tests.sh: 17 comparisons.
- Also translated now instead of interpreted in the block (found by listing, per system program, which instruction
  forms are interpreted in blocks and how often): `shrw/shlw/sarw $n,%reg`, `incw/decw %reg`, `movw $imm`,
  `rol/ror $n` (32 bit; if CF or OF is read afterwards, the interpreter computes it - `g_afterwards_now`),
  `xchg [m],r` (locks; without a second thread, load and store suffice). Remaining: `cld` + `rep cmpsb` in dyld
  (16-59 times per run) and the system calls.

**Signals and FPR/VR (2026-11-14, night).** With the sampling profiler, 1-6 % of the runs aborted (the host jumps to
`0x7fffdeac`, `0x0fffdeac`, `0x124bc194`); without signals 0 of 200. `0x7FFFDEAD` is the kernel's fill value for
vector registers, `0xC24BC195 87859393` the one for floating point registers: after signal delivery, registers that
were not saved apparently come back with fill values. The translator uses only v0-v3 (reported in VRSAVE). A small
experiment, `tools/probes/signal_regs.c` (checks f14-f31 and v0-v3 under SIGALRM), is ready. Important for the
"signals" stage.
Result (2026-11-14, early morning): in plain C, f14-f31 and v0-v3 survive 115000 signals (VRSAVE 0xf0000000 and
0xffffffff) without a single wrong value. So the simple theory is wrong; it is open whether signals during system
calls or in the translator itself (possibly registers outside VRSAVE) cause the damage.

**Kernel panic (2026-11-14, night)** ("zalloc: zone kernel map entries empty", the zone has a fixed 40215 entries).
Cause: ITIMER_REAL every 29 us. The kernel processes the timer callbacks in its own threads and creates new ones when
they back up; the first dense run raised the zones `threads` to 5330 and `kernel_stacks` to 5236 (83 MB wired), every
kernel stack occupies one entry of the kernel map, and nothing is returned. If a process crashes with the 29 us timer
running, ReportCrash stops it for seconds and the callbacks keep piling up until the zone overflows. Countermeasures:
the fault handler stops the sampling timer first, `ROSETTA_HPROFILE_US` not below 50 us. `tests/zones.sh` and
`tests/kmap.sh COMMAND` show the usage (~360 right after system start).

For bench_libm the instructions of the Intel libm count (interpreter); with stubs they no longer run at all, so the figure
is "Intel instructions per second". A good half of the time there is spent in the native libm itself.

Startup of `hello`: 0.17 s (fastest of 5, `tests/startup.sh`; interpreter only 0.15 s). Tests: `tests/run_tests.sh`
(15 comparisons against native, real Intel, interpreter and ls; `array_end` checks prefetching past the end of a
page). Running measurement series: `tests/measure_all.sh`.

**Added since 2026-11-13 evening** (each with a switch to disable it):
- *Hoisting array accesses* (`ROSETTA_NO_PREFETCH`): loop blocks with exactly one backward jump, address registers
  with a fixed step and no memory stores load doubles four iterations ahead into a ring buffer (128 bytes per block,
  pointer in a free register of r28-r31); the fault catcher skips prefetches past the end of an array
  (`jit_prefetch_error`).
- *Floating point loads via AltiVec*: `lvx`/`lvx`/`vperm` (swap pattern from Cpu.vswap8/vswap4 selected by a & 15)
  /`stvx` to Cpu.fp_store, then `lfd` - ~31 instead of ~63 cycles with a dependent computation (small experiments
  `tools/probes/lhs*.c`).
- *Floating point store forwarding* (`ROSETTA_NO_FW`): doubles/floats stored by the block itself are kept in f2-f13.
- *libm direct* (`ROSETTA_NO_NATIVE`): the stub block fetches the arguments via AltiVec into f1/f2, calls the PowerPC
  function and puts f1 on st(0) - without a C wrapper (bench_libm +16 %).
- *mov fusion*: `mov r,s` before an operation on r becomes a three-operand instruction.
- *Embedding calls* (`ROSETTA_NO_EMBED`): direct `call`s to functions whose `ret` comes within <= 24 instructions
  without a further call/indirect jump are read on inside the block (two levels); `call *` (function pointer) as
  well, if the target can be predicted at translation time from guest registers/memory (a guard compares; a miss
  executes the call and retranslates the block without the prediction, `Cpu.spec_miss`). The embedded `ret` checks
  the return address against the expected one.
- *Forwarding through the Intel stack* (`ROSETTA_NO_SW`): esp/ebp tracked symbolically relative to the block head;
  push/mov to stack slots are recorded as a constant, guest register or copy in a free register of r28-r31;
  pop/leave/mov loads fetch them without memory, the embedded `ret` disappears entirely if its slot provably holds
  the address.
- *Conditions from stored flags* without a C helper, when the flag kind at run time is cmp/sub or logic (32 bit)
  (qsort: `jge` on a `jne`; bench_control_flow +9 %).
- Bug fixed: `writes_reg` reported no written register for `mul/imul/div/idiv` (f6/f7 /4-7) and `not/neg r/m8`
  (today `write_mask`, a mask in one pass).
- *libm directly at the call site*: `thunk.c` reads the stub sections of every image (S_SYMBOL_STUBS, also the
  `__IMPORT` jump table) and their symbol names via LC_DYSYMTAB; `call stub_sin` is then generated without the stub,
  shadow stack and detour through Cpu.st (arguments from the forwarding or via AltiVec, result directly in the block's
  x87 stack). This also works before dyld binds the entry. bench_libm +8 %.
- *Hoisting conversions*: `cvtsi2sd` of a loop counter (also after `movzbl`/`movzwl`) puts the bits
  (2^52 + 2^31 + value) into the ring buffer four iterations ahead; such streams read no guest memory, so they may
  also run in loops with memory stores. bench_float +15 %.
- *Fine-tuning* (night of 2026-11-13/14): more string stubs (strtok, strtok_r, strspn, strcspn, strpbrk, strncat,
  strcasecmp, strncasecmp, atoi, strtol, strtoul; nothing with malloc - guest and host heaps are separate; the
  guest's errno stays untouched), argument stores `mov [esp+d],reg` as a copy in r28-r31 for the stack forwarding
  (`ROSETTA_SW_ARG=0` disables it), floating point forwarding with host copies (Cpu.fw_shadow, or the storage slot of
  a pure copy): before a C call without guest memory access (libm, sqrt; `g_c_pure`) the live entries are saved and
  afterwards fetched with `lfd` instead of an AltiVec swap. Page protection cache 64 instead of 8 ranges. Block size
  checked (`ROSETTA_MAX_INSNS`): 48 stays best (80/120: bench_float -10 %).
- Where it is still stuck: the bench_integer sort loop (x86 spills six values, write-through + range check), the switch
  dispatch in bench_control_flow (latency chain up to `bctr`, as native), `fdiv`/`fadd` chains in bench_float, indirect calls with
  changing targets (`bench_calls`, third loop).
- Tried without measurable gain (disabled or removed again): own switch jump tables with host addresses
  (`ROSETTA_JUMPTAB=1`; the dispatch depends on the `bctr`, as native), results of read-modify-write instructions in
  the stack forwarding, `setcc` via `mfcr` (faster only with random conditions), `cvtsi2sd` via two float halves
  (slower: `stw`->`lfs` is not forwarded either).

**Stubs for the libSystem** (`thunk.c`): at the first block in an unknown memory range, `vm_region` looks up the
range; if it starts with an i386 Mach header, `__TEXT` is recorded, and for the libSystem (LC_ID_DYLIB) the symbols
are read from `__LINKEDIT` in memory. Names with a suffix (`$fenv_access_off`, `$UNIX2003`) count like the base name.
At the entry address there is then a stub block instead of translated Intel code: store esp, call the C function
(arguments from the Intel stack, double/float result to st(0), integer/pointer to eax), then `ret` as translated.
Replaced: 60 math functions (double and float, sin to hypot, pow, atan2, fmod, ldexp) and strlen, strcmp, strncmp,
memcmp, strcpy, strncpy, strcat, strchr, strrchr, memchr, strstr. `ROSETTA_NO_STUB=1` disables them.
Consequences: results as with the PowerPC libm (bench_libm bit-identical to the native build), the math functions do not
set the guest's errno, rint/nearbyint always round to nearest (the guest's rounding mode is ignored).

**PIC call** `call 1f; 1: pop ebx` (start of almost every i386 function with global data) is translated as `push`:
before, one entry per call stayed on the shadow stack, the ring ran through cold memory and every `ret` of these
functions missed it (bench_control_flow and bench_libm +2-3 % each, bench_integer unchanged). `ROSETTA_NO_PIC=1` for comparison.

**Floating point native**: scalar SSE2 in f14-f21 (xmm0-7, low element), x87 as a per-block stack in f24-f31,
compares (ucomisd, fucomi) together with their readers as fcmpu. The G4 cannot load floating point values
byte-reversed: values go through lwbrx/stw and lfd; the lfd waits until the value is needed. Pure copies (movsd load
and store again, fstpl of a value that is only in Cpu.st) run entirely without the FPU through integer registers.

**What load-after-store costs on the 7447A** (`tools/probes/lhs1.c`, `lhs2.c`, cycles above an empty loop): lfd
after two stw to the same address ~22; a distance of 6 or 12 instructions changes nothing, only ~40 cycles (or a
prefetch four loop iterations ahead) remove the penalty. Equal size is cheap: stw->lwz, stw->lfs, stfd->lfd ~8,
stfd->lwz lwz ~12. The detour via AltiVec (lvx/vperm/stvx/lfd) is hardly better. So every double from memory costs
~22 cycles, unless the value is already known at translation time.

**Predicting constants**: if a block reads floating point values through a register it never changes (PIC base
`ebx`, also via `lea eax,[ebx+d]` or `mov`) from a read-only region, the translator reads the value already at
translation time (the guest registers are then in `Cpu`; every path into `translate()` goes through the exit). The
value goes into a constant pool in the host (16384 entries, discarded when the code cache is flushed) and is loaded
with `lis`+`lfd`. A guard at the block start (before the loop head) compares the register with its value at
translation time; if it fails (`KIND_WATCHDOG`), the block is retranslated without the prediction and the old one is
redirected there. `xorpd`/`andpd` (also `ps`) with a sign or absolute value mask become `fneg`/`fabs` on the FPR; the
high half of the register gets the same operation in memory. bench_float +15 %, bench_libm +2 %. `ROSETTA_NO_CONST=1`
disables it.

What the translator does:
- **Superblocks**: conditional jumps are side exits, unconditional forward jumps are followed; a backward jump to the
  block start goes there directly. Chained exit stubs also redirect the `bc`/`bl` before them directly to the target.
- **Flags**: exact liveness with a look into the following blocks; at call/ret and jump tables they count as dead
  (calling convention; `ROSETTA_CAREFUL=1` disables this). Flag setter + reader become one compare in CR0, also for
  several readers in a row (`cmp; jl; setg`). `ROSETTA_NO_FUSION=1` for testing.
- **Stack slots in loops**: up to 4 frequently read [ebp/esp+d] in r28-r31, writes go through to memory, other memory
  stores check against the range (r2) and reload. Free registers hold loop constants. `ROSETTA_NO_SLOTS=1` for
  testing.
- **Return stack**: ring buffer in r13 (64K entries); `call` stores (Intel address, continuation) and jumps with `bl`,
  `ret` compares and returns with `blr` (link stack of the G4). Indirect jumps with an inline cache.
- **Interpreting within the block**: the block calls unknown instructions through `step()` without leaving.
- Native: mul/imul/div/idiv (special cases in the interpreter), bsf/bsr, SSE2 integer on AltiVec (movdqa/movdqu,
  pcmpeqb/w/d, pand/pandn/por/pxor, pmovmskb). VRSAVE is set at entry.
- Tools: `ROSETTA_STATS=1` (counters, interpreted opcodes, dispatcher entries), `ROSETTA_PROFILE=1` (samples,
  `tests/profile.sh`, `tests/show_block.sh EIP`, `tests/host_sym.sh ADDR`), `tools/commpage_users.sh LIB` (which
  commpage routines an Intel library jumps to).

Old bugs found: the commpage `memset_pattern` (+0x1000) was missing, memset with a value != 0 from 256 bytes on wrote
nothing; now native, and an empty commpage slot aborts with a message. `raw_arith` read the carry after a ret (test
corrected, reference regenerated with Zig).

Limits of `bench_integer`: at native level since the big rework (102 %). The sort loop still needs 14 instead of ~6 PPC
instructions (register copies instead of renaming; lwbrx/stwbrx have no displacement form).

Limits in floating point: every double from arrays (matrix product in bench_float: 21 % of the time) costs the ~22 cycles;
only hoisting across loop iterations would help (software pipelining, guarding reads before the end of the loop).
Values the block stored itself (intermediate values around calls, n-body) could be taken from the FPR by a per-block
store forwarding; expected gain a few percent.

Next steps: store forwarding for floating point, hoisting array loads, cvtsi2sd via lfs (same size), fldt/fstpt
(m80), then stage 4. (Packed SSE2 doubles, signals and threads: done, see above.)

## Stage 5: graphical Intel programs (from 2026-11-14, evening)

**Access to the screen.** Graphical programs run through `tests/in_session.sh SCRIPT` in the login session
(`launchctl submit` via the session's launchd, `LAUNCHD_SOCKET`, without root). Checking results without capturing
the rest of the screen: `tools/screen_grab X Y W H [png]` reads exactly one rectangle from the framebuffer (probe
window on the top level) and wakes a sleeping display for it (`UpdateSystemActivity`), then lets it sleep again
(`IORequestIdle`). With a sleeping display the WindowServer does not composite an image;
`CGWindowListCopyWindowInfo`/`CreateImage` return nothing on this system, `screencapture -l` does not exist under
10.5.

**Results.** `tests/graphics1.c` (CGS window, CoreGraphics draws four stripes) and `tests/graphics2.m`
(NSApplication, borderless NSWindow, own NSView, event loop) run pixel-identical to native under rosetta: Intel's
CoreGraphics talks to the PowerPC WindowServer (it knows a WindowServer with the tag `'ppc '` and byte-swaps
itself), events, timers and drawing work.

**What was needed:**
- Replacement libraries (`tools/intel_replacements.sh`, C for i386, translated by rosetta): vImage (15 functions that
  libraries in the root import; premultiplication exact, scaling bilinear), libBLAS (6 float routines), libLAPACK (2
  routines report an error). The Combo Update did not update them, but Accelerate/vecLib load them.
- Stub libraries (`tools/intel_stubs.sh PATH...`, `app_stubs.sh NAME`): for every missing i386 library (CoreVideo,
  SpeechRecognition, CarbonSound, SecurityHI, InstallServer, SpeechObjects, Calculate) one with the install name and
  the versions the clients expect; functions report "missing" on the first call and return an error code, keys bound
  at load time become CFStrings. `tools/deps.sh` finds the gaps.
- Targeted patches in the root (`tools/intel_patch.sh`, checks the original bytes, backs up to `intel/backup/`; run
  again after every rebase/prebinding): (1) CoreGraphics 409.3 against WindowServer 409.8: allow the version mismatch
  (otherwise exit(1) in CGSServerPort). (2) Security: send the ClientSetupInfo version so that securityd, in its
  byte-swapping mode, reads it raw as 0x4e20 (otherwise CSSMERR_CSSM_INCOMPATIBLE_VERSION, LaunchServices aborts).
  (3) LaunchServices waits forever without `IODeviceTree:/efi` (PowerPC: Open Firmware); the wait is skipped.
- System calls: getfsstat (18), getattrlist (220) and getdirentriesattr (222) with byte-swapping of the packed
  attributes by a type table (Finder info and strings raw).
- Hardening: predictions at translation time read guest memory with `vm_read_overwrite` (the host's shared library
  region has holes, vm_region still reports it as readable); memory calls of the guest (munmap, mprotect, fixed mmap,
  vm_deallocate/-protect) in the range 0x90000000-0xb0000000 are refused.
- Guest mach_msg interruptible: the host wrapper silently restarted an interrupted receive, so a signal to a program
  in the event loop never arrived. Now with MACH_RCV_INTERRUPT; without a pending guest signal it retries, with a
  signal it halts all block starts and reports MACH_RCV_INTERRUPTED (the guest wrapper retries, ends up in the
  dispatcher on the way, and the dispatcher delivers the signal).
- Calculator.app: `tools/intel_app.sh NAME` assembles the bundle from /Applications (resources) and the i386 files of
  the update (intel/apps/); the update contains only changed files.
- Debugging graphical programs: the fault handler first writes without stdio (signal, address, srr0, lr) and shows
  the last memory calls of the guest with the ebp chain (ring buffer in sys.c); `ROSETTA_STATE=1` + SIGUSR2 shows the
  guest state at the dispatcher; `tests/app_sample.sh` (Apple's sample), `tools/host_symbol`, `libsystem_sym.sh`
  (local symbols of the host libSystem), and a search for the code that accesses a given global variable (PIC).

**Fonts (ATS).** Intel's ATS talks to the system's ATSServer (PowerPC) in its own Mach protocol without MIG:
FontMgrMessage, 1740 bytes, `msgh_id` = message type (in the reply: status), followed by data fields raw in the
sender's byte order, at the end client ID (0x6c0), server generation (0x6c4) and a byte order byte (0x6c8: Intel 1,
PowerPC 0). Apple's Intel ATSServer byte-swapped for PowerPC clients under Rosetta; in the PowerPC server this spot is
empty (three `nop`s in the function that fills the sync block), and the Intel server is encrypted (DSMOS, not
circumvented). rosetta therefore converts every message to the font server (`mach_msg.c`, port taken from the
bootstrap look_up reply for `com.apple.FontObjectsServer`):
- Layout table per type (request and reply), read off Intel's client (`tools/ats_layout.pl` lists the buffer accesses
  of each send function) and checked against the PowerPC server (`tests/ats_ppc_range.sh`, jump table of the
  server's dispatcher at 0xc8dd0). The server replies in the request's buffer: whatever it does not overwrite is
  swapped back as well. rosetta reports types without a layout once ("content stays raw").
- Sync block (reply with 8 OOL descriptors): 32-bit values, except 0x80 (64-bit address of the database in the
  server, from which `gBaseAddressDelta` is derived), 0xa8 (16 bit) and 0x116 (RelativePtr, which the client swaps
  itself). The shared database itself (big-endian) is read by the client with its run-time byte swapping (built for
  Apple's Rosetta); the client enables it because the rendezvous (type 0) reports the server's other byte order in
  byte 0x40.
- MapSharedMemory (0x15): 64-bit server address at 0x38. Errors here made the server stack up mappings per client
  until it reported -108 (at most 10).
- OFA messages (glyphs, 0x1f-0x21): content behind the descriptors (the count word at 0x18 is present even without
  the COMPLEX bit), data as attribute groups {type, length, check value, count; attributes ID, size in words with
  padding bytes in the top 2 bits}. rosetta walks them recursively: words, except 'gary' (glyph numbers, 16 bit) and
  'dest' (two 64-bit addresses in shared memory). The OOL region of the request is copied and the copy swapped (guest
  memory stays untouched; deallocate semantics emulated).
- Results in the client's shared memory ('dest' = 'shmo'): the server writes them there in its byte order; rosetta
  swaps them after the reply according to the result kind: 'mfrc' (24 bytes Fixed), 'mrdr' (36 bytes, two 16-bit
  fields height/width at +0x10), 'path' (words). rosetta reports other kinds once.
- Patch 4 from the preparatory work (forcing run-time byte swapping) has been reverted: no longer needed.

Result: Calculator.app (Intel) draws windows and text completely; `tests/fonts.c` (CoreText: metrics, glyphs,
advances, outlines, paths for three fonts) gives the same output under rosetta as native (test suite, needs the login
session). Debugging: `ROSETTA_ATS_LOG=1` shows every message in server byte order with OOL and destination buffers
(`tests/ats_trace.sh`, `ats_sequence.sh`, `ats_dest.sh`, `ats_dest_types.sh`); guest crashes now show the stack and
the ebp chain with arguments. Open: GetFSRefForFileToken (0x09, FSRef raw: "ATSFontGetFileReference failed"),
requests with embedded structures (0x02, 0x0a, 0x12, 0x14, 0x18, 0x1b) and bitmap results.

**Transmission 2.42** (screenshot `docs/transmission-on-ibook-g4.png`). Last version for Leopard, official download
from github.com/transmission/transmission-releases (4089162 bytes, MD5 2b2ab8cc42b96242a1bc7470c5a20123), main
executable x86_64 and i386 only. `tools/thirdparty_unpack.sh DMG NAME` mounts the image read-only and copies the
bundle to intel/apps/. Before the first start, `tests/transmission_setup.sh`: no port forwarding on the router
(NatTraversal off), no update check (Sparkle). Only libraries were needed:
- Quartz umbrella shell (`intel_umbrellas.sh Quartz`: re-exports QuartzCore, QuartzComposer, PDFKit, ImageKit).
- Stub libraries for QuickTime, QTKit, QuartzFilters, MonitorPanel (ImageKit links them). QTKit provides class
  symbols of the old objc runtime (`.objc_class_name_QTMovie`): as absolute symbols with value 0 (as in the real
  library).
- `tools/stubs_rebuild.sh` builds all stub libraries with the symbols of all programs at once (QuartzComposer needs
  more keys from the CoreVideo stub library than Calculator). Stub libraries carry an identifying string (also those
  without functions); `deps.sh` counts them as missing with `STUBS_REBUILD=1`.
Window, toolbar, status bars and welcome dialog draw completely. Operation: `tests/app_start.sh NAME` (keeps running,
output in /tmp/app_NAME.log), `app_stop.sh NAME`, `app_windows.sh NAME`. For the screenshot:
`tests/proof_terminal.sh` opens a Terminal window with version, `file`, hardware, `machine` and the rosetta process
(`proof_show.sh`), `capture_rect.sh` captures each window separately. Limits: accessibility (System Events) sees no
windows of a program under rosetta; kqueue (362) is missing, libevent falls back to poll/select (kqueue since
2026-11-15; it uncovered the AppKit EFI hang described under "Robustness").

**Robustness** (2026-11-15, night). Starting point: of the 338 BSD calls of Leopard, 121 were handled. Now:
- Processes. `fork`/`vfork` fork the host (its libc prepares malloc); the child resets the thread lock and the
  watchdog (`thread_after_fork`). i386 convention: edx = 1 and eax = own pid in the child, edx = 0 and eax = child pid
  in the parent. `execve` reads the file: PowerPC-capable (thin ppc, fat with ppc, scripts, anything else) goes to the
  kernel; Intel-only (thin i386, fat without ppc) is started through rosetta again (`-0 argv0 -- path ...`). The
  host signal mask becomes the guest mask before the exec, so the new program inherits it as natively; `sys_init`
  turns it back into a guest mask and takes over ignored signals. `posix_spawn` (10.5 descriptor: attr flags at 0,
  sigdefault 4, sigmask 8, pgroup 12; file actions of 1040 bytes: type, fd, oflag/newfd, mode, path at 14) forks, runs
  the file actions, reports a failed exec through a close-on-exec pipe.
- The Leopard kernel refuses `execve` with ENOTSUP while the process has a second thread. As soon as a program had a
  signal handler, rosetta's watchdog thread was that thread (the gcc driver failed to start cc1). The watchdog now
  waits on a condition variable instead of usleep and is stopped (joined) before every exec.
- Signals. The guest mask exists only in rosetta; a SIG_DFL signal the guest blocked used to kill the process at once.
  `sig_sync` installs the catcher for exactly those signals; the default action happens when the guest unblocks
  them. `sigpending`, `__sigwait`, `__pthread_sigmask`. Waits with several threads use a wake-up pipe (written by the
  host handler) instead of polling every millisecond; its descriptors live high and are protected from the guest's
  `close`/`dup2`. Exceptions found by the interpreter (division error, ud2, int3, hlt) reach the guest handler
  (`RUN_SIGNAL`: eip stays at the instruction, the dispatcher builds the frame); without a handler rosetta ends with
  the signal itself, as do memory faults.
- `kqueue`/`kevent` (struct kevent is 20 bytes on both sides, converted field by field), real `fcntl` locks (struct
  flock), F_PREALLOCATE, F_RDADVISE, F_LOG2PHYS, tcsetattr, POSIX semaphores, chown/chflags/truncate/mkfifo/futimes,
  setattrlist (the getattrlist field table: swapping is the same in both directions), sendfile, the `*_extended`
  calls, `map_fd`, csops, iopolicysys. `__pthread_canceled` (333) had been handled as `__semwait_signal`.
- sysctl structures: kinfo_proc (492 bytes; the own process reports the guest's name and arguments, also through
  KERN_PROCARGS2), routing messages (NET_RT_IFLIST for getifaddrs: if_msghdr, ifa_msghdr, rt_msghdr, sockaddr_dl
  index, sin6_scope_id), loadavg, xsw_usage, clockinfo. A size query (no buffer) passed no length pointer to the
  kernel: the guest got its own (often uninitialized) value back; getifaddrs crashed in mailq and showmount.
- The processor. CPUID and the commpage already described a Core Duo; hw.cputype/cpusubtype/cpufamily, hw.optional.*,
  machdep.cpu.* (fake OIDs for sysctlbyname) and host_info(HOST_BASIC_INFO) now agree. full2tt6e had refused to run on
  an "unsupported architecture"; `arch` prints i386 as on an Intel Mac.
- `IODeviceTree:/efi`. AppKit, in `[NSApplication _taskNowMultiThreaded:]` (when the first NSThread starts), looks up
  this node with io_registry_entry_from_path and, if it is missing, waits forever on a freshly initialized local
  condition. Transmission hit it as soon as kqueue worked (UKKQueue starts an NSThread) and hung for minutes. The
  request is now answered with the device tree root (path rewritten, MIG size and count adjusted).
- Intel root redirection only for dyld when the system file has an i386 part itself: `file`, `nm`, `lipo` and copy
  programs saw thin copies instead of the real universal files. Writes never go to the root.
- Calculator: CalculatePerformExpression (private Calculate.framework, i386 only as a stub library) runs as the native
  PowerPC function; native replacements now also cover named frameworks besides libSystem (`g_native_libs`).
- `tests/cli_matrix.sh`: every universal program of /bin and /usr/bin without arguments plus ~120 workloads, native
  before and after, translated in between. Pitfalls in the harness itself: `grep -v` adds a final newline, the
  shell's `echo` interprets `\n`, a shell wrapper reorders the environment and sets `_`, Leopard has no pgrep/pkill
  and `nohup` dies with the SSH session (perl setsid instead).
- Translation cache checkpoints at 6 s and 45 s; programs with windows are usually closed by a signal, so their cache
  was never written. A run writes again only with substantially new code.
- Prefetch ring for floats and for loops with stores whose address registers match the stream (a store hits a
  prefetched value only if dd - k*step overlaps it for k between the prefetch and the use). Measured on the G4
  (`tools/probes/swap_load_cost.c`): lwbrx costs barely more than lwz; four word loads, four stores and two lfd per
  two doubles take 20 cycles, lvx/vperm/stvx per double 16, one vector for two doubles 14.

**Translation cache across library layouts** (2026-11-15). Programs with windows alternated between a cold start
and a lost cache: in the second run, Calculator loaded 23497 blocks, but 26 mappings did not match the saved run, the
first one flushed everything, the cache was deleted, and the third run was cold again (Transmission likewise with 43).
The cause is the small libraries that are not prebound at a fixed address (stub libraries, frameworks inside the
application): dyld places them wherever there is room, and that differs between a cold and a warm start.
- Dependencies per block (`BlockDep`, `dep_note`): besides the global page bitmap, each block records the guest pages it
  was translated from (its instructions, inlined callees, predicted constants) as up to three spans of pages; spans
  within 64 KB merge, a fourth region widens the nearest span, beyond 16 MB the block counts as relying on everything.
  The records follow the order of the code, so a binary search by code offset finds them; 20 bytes per block, also in
  the cache file.
- A mapping, munmap or mprotect over pages with code no longer flushes but queues the range (`jit_range_changed`,
  `g_flush` 2); the dispatcher then discards exactly the blocks whose spans touch it (`invalidate_ranges`): the first
  instruction of each becomes a jump to an exit stub with its eip, which chains to the new translation at its first
  use (jumps already chained to the old block keep working), map and jump table forget it, variants get a tombstone
  (probe chains stay intact; the watchdog skips them), the return rings start anew, the code pages of the range are
  free again. Too many ranges at once (64) or no room for the stubs: flush as before. `ROSETTA_NO_PARTIAL=1`.
- Checking a mapping per segment: the cache now stores every mapping of the run (address, file, offset in the file;
  anonymous ones too), and a new mapping counts as expected only if the newest saved one at that place had the same
  part of the same file there. The old check per image (same file somewhere in the old image range) let through a
  framework that dyld had placed two pages further on: same file, but different code at every address. Segments do
  not keep their file distance in memory (libSystem's `__DATA`), so the offset is compared per mapping, not per image.
- A mapping is also not expected when code translated in this run relies on its pages (`g_newpages`): the program in
  `tests/remap.c` maps file 2 over file 1 and file 1 back again, which matches the saved run, but the blocks there came
  from file 2. The test also checks a caller in another file that inlined the replaced code: with the translation
  cache of the run before, exactly that caller block is discarded (`ROSETTA_CACHE_SHOW=2` lists the discarded blocks).
- Blocks outside the images were written with an exit of kind INVALID at their start; every jump into them then
  translated the block again. Now it is a chainable exit stub like above. The cache is written again when more than
  32 blocks from it had to be discarded, so the next run finds the new layout. `ROSETTA_CACHE_SHOW` and `ROSETTA_LOG`
  had changed the key of the translation cache (strncmp with the wrong length): a measurement with them used a cache
  of its own. `ROSETTA_CACHE_FILE` sets the file (for tests). Discarded blocks lead to a rewrite only beyond 32 +
  1/64 of the cache, like new code: Calculator discards 161 blocks in every warm start (two allocations land on pages
  of the saved run), translating them again costs ~25 ms, rewriting 16 MB right after the window appears more.
- Every program has a translation cache of its own; after the test matrix the folder held 533 of them (200 MB).
  Beyond `ROSETTA_CACHE_MAX_MB` (default 256) together, the caches used longest ago are deleted when one is written.

**Memory faults to the program's handler** (2026-11-15). Until now a SIGSEGV/SIGBUS of guest code ended the program
even when it had a handler (crash reporters, runtimes that recover from faults).
- The host fault handler (main.c `memfault`) asks `jit_fault_state` whose fault it is: in the code cache a translated
  block (the guest registers are in r14..r21 of the host context, eip = start of the block from the dependency
  records, which are in code order), with `g_in_code` a native function called from a block (eip = the block that
  called it, found through lr or the frame chain of the host stack), with `g_guest_exec` the interpreter between
  instructions (exact). `g_guest_exec` is off during system calls: a bad pointer there is the translator's business.
  If the guest has an unblocked handler (`sys_fault`), the signal becomes pending with si_addr and si_code of the
  host, and `_longjmp` leads back to the dispatcher of this thread (`dispatcher` sets the jmp_buf once per call,
  not per round; `run` likewise for the interpreter alone), which unblocks SIGSEGV/SIGBUS in the host and delivers.
- Inside a translated block the exact instruction is not known, nor are lazily kept flags, deferred stack slots and
  floating-point values in host registers: a precise state would need metadata per instruction (deoptimization).
  Such a delivery is marked in the frame (reserved area of the float state); if the handler returns to the same
  eip, `sig_back` ends the program with the signal, as natively a handler that gives up does (the instruction would
  fault again), instead of running on with a wrong state. Ending, siglongjmp and continuing elsewhere (changed eip
  in the context) work. `tests/faults.c`: write to a read-only page, read of a page without access and of address 8,
  a hot loop that runs translated over the end of its mapping (1024 elements, then the fault in the block), a
  handler that continues in another function; `faults retry` (make the page writable and return) repeats the write
  in the interpreter and ends with SIGBUS (138) when translated.
- Unmapped memory makes no reliable test: guest and translator share the address space, the host can reuse a page
  the guest gave back right away. The first version of the test read after `munmap` and hung there; the hot loop
  without `volatile` hung too (gcc drops the loads of a function that never returns).
- The interpreter's push stores before it moves esp: a fault on the stack page leaves esp unchanged, as on x86.
- sysctl: `struct vfsconf` (40 bytes, getvfsbyname, lsvfs: mount count and the "local" flag were swapped) and
  `struct nfsstats` (ints; nfsstat found no NFS type number before, because it reads it from vfsconf). `full2tt6e`
  is not a bug: its i386 part is a placeholder of Apple's that prints "unsupported architecture" and exits.
- Result, window after start (first, second, from the third start with an empty cache): Calculator 9.3 / 8.8 /
  2.8-3.3 s (the native PowerPC Calculator 2 to 3 s), Transmission 17.0 / 17.4 / 9.5-11.1 s. The second start
  translates everything that the first one only interpreted (threshold 1 with a cache: 37600 blocks for Calculator),
  from the third on almost nothing is translated (450 blocks) and only a few blocks are discarded (57 for an
  allocation that lands on pages of the saved run). Loading Calculator's cache (60817 blocks, 14.6 MB code): 47 ms,
  1494 page faults. Processor time profile of a warm Transmission start: translated guest code 36 %, interpreted 5 %,
  system calls and host libraries 54 % (mach_msg 20 %, stat/open/close/mmap/getattrlist together about 15 %),
  loading the cache 1.7 %, discarding 0.8 %, translating 0.5 %.
