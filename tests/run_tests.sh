#!/bin/sh
# run_tests.sh - translator against references: native PowerPC build of the same program, real Intel output, ls.
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
ulimit -t 120
failed=0
same() {        # name, rosetta command, reference command
    "$2" > /tmp/pr_a.txt 2>&1; "$3" > /tmp/pr_b.txt 2>&1
    if cmp -s /tmp/pr_a.txt /tmp/pr_b.txt; then echo "ok      $1"; else echo "FAILED  $1"; diff /tmp/pr_a.txt /tmp/pr_b.txt | head -5; failed=1; fi
}
t_raw()   { ./rosetta ./raw_arith; }
r_raw()   { cat ../tests/raw_arith.expected; }
t_cond()  { ./rosetta ./conditions; }
r_cond()  { ./conditions.ppc; }
t_int()   { ./rosetta ./bench_integer 2; }
r_int()   { ./bench_integer.ppc 2; }
t_hello() { ./rosetta ./hello a b; echo "code $?"; }
r_hello() { ./hello.ppc a b | sed "s/hello.ppc/hello/"; echo "code 7"; }
t_ls()    { ./rosetta /bin/ls -la / /usr/lib; }
r_ls()    { /bin/ls -la / /usr/lib; }
t_lsr()   { ./rosetta /bin/ls -laR /usr/share/man/man1; }
r_lsr()   { /bin/ls -laR /usr/share/man/man1; }
t_sse()   { ./rosetta ./sse_int; }
r_sse()   { ./sse_int.ppc; }
t_t2()    { ./rosetta ./bench_control_flow 1; }
r_t2()    { ./bench_control_flow.ppc 1; }
t_mem()   { ./rosetta ./memtest; }
r_mem()   { ./memtest.ppc; }
t_str()   { ./rosetta ./str_list; }
r_str()   { ./str_list.ppc; }
t_t3()    { ./rosetta ./bench_float 2; }
r_t3()    { ./bench_float.ppc 2; }
t_t4()    { ./rosetta ./bench_libm 2; }
r_t4()    { ./bench_libm.ppc 2; }
t_t4o()   { ROSETTA_NO_STUB=1 ./rosetta ./bench_libm 2; }
r_t4o()   { ROSETTA_INTERP=1 ./rosetta ./bench_libm 2; }
t_ae()    { ./rosetta ./array_end; }
r_ae()    { ./array_end.ppc; }
t_fpc()   { ./rosetta ./fpcompare; }
r_fpc()   { ./fpcompare.ppc; }
t_w16()   { ./rosetta ./word16; }
r_w16()   { ./word16.ppc; }
t_sed()   { ./rosetta /usr/bin/sed -e 's/a/b/g' -e 's/^#.*//' /etc/services; }
r_sed()   { /usr/bin/sed -e 's/a/b/g' -e 's/^#.*//' /etc/services; }
t_sig()   { sh ../tests/with_timeout.sh 20 ./rosetta ./signals; }
r_sig()   { ./signals.ppc; }
t_sige()  { (./rosetta ./signal_exit; echo "code $?") 2>/dev/null; }     # hides the "Terminated" message of the shell
r_sige()  { (./signal_exit.ppc; echo "code $?") 2>/dev/null; }
t_cf()    { ./rosetta ./cf; }
r_cf()    { ./cf.ppc; }
t_objc()  { ./rosetta ./objc_plain; }
r_objc()  { ./objc_plain.ppc; }
t_fnd()   { sh ../tests/with_timeout.sh 120 ./rosetta ./foundation; }
r_fnd()   { ./foundation.ppc; }
t_thr()   { sh ../tests/with_timeout.sh 60 ./rosetta ./threads; }
r_thr()   { ./threads.ppc; }
t_thrns() { sh ../tests/with_timeout.sh 60 ./rosetta ./threads_ns; }
r_thrns() { ./threads_ns.ppc; }
t_intr()  { sh ../tests/with_timeout.sh 30 ./rosetta ./interrupt; }
r_intr()  { ./interrupt.ppc; }
t_ctrlc() { sh ../tests/ctrl_c.sh | sed 's/ after .*//'; }
r_ctrlc() { sh ../tests/ctrl_c.sh ./ctrl_c.ppc | sed 's/ after .*//'; }
t_t5()    { ./rosetta ./bench_sse2 3; }
r_t5()    { ./bench_sse2.ppc 3; }
t_t5s()   { ROSETTA_THRESHOLD=1 ROSETTA_FPAHEAD=1 ./rosetta ./bench_sse2 3; }
t_t5o()   { ROSETTA_THRESHOLD=1 ROSETTA_NO_XFH=1 ./rosetta ./bench_sse2 3; }
t_pdo()   { ROSETTA_THRESHOLD=1 ./rosetta ./pd_ops; }
r_pdo()   { ROSETTA_INTERP=1 ./rosetta ./pd_ops; }
# second run from the translation cache: the vperm patterns of the shuffles must come back with the code
t_vec()   { rm -f /tmp/rr_vec_cache; for t in 1 10; do ROSETTA_CACHE_FILE=/tmp/rr_vec_cache ROSETTA_THRESHOLD=$t ./rosetta ./vec_ops; done; rm -f /tmp/rr_vec_cache; }
r_vec()   { ROSETTA_INTERP=1 ./rosetta ./vec_ops; ROSETTA_INTERP=1 ./rosetta ./vec_ops; }
t_vd()    { ./rosetta ./vdsp_d; }
r_vd()    { ROSETTA_NO_CACHE=1 ROSETTA_THRESHOLD=2 ROSETTA_ONLY=0-0 ./rosetta ./vdsp_d; }    # interpreted, native math as above
t_sys()   { sh ../tests/with_timeout.sh 60 ./rosetta ./syscalls; }
r_sys()   { ./syscalls.ppc; }
t_flt()   { sh ../tests/with_timeout.sh 20 ./rosetta ./faults; }
r_flt()   { cat ../tests/faults.expected; }
t_flti()  { ROSETTA_INTERP=1 sh ../tests/with_timeout.sh 20 ./rosetta ./faults; }
t_fltj()  { ROSETTA_THRESHOLD=1 sh ../tests/with_timeout.sh 20 ./rosetta ./faults; }
# a handler that makes the page writable and returns: repeated exactly by the interpreter; translated the state is
# approximate, the program ends with SIGBUS (138) after the handler instead of running on with a wrong state
t_fltr()  { ROSETTA_INTERP=1 sh ../tests/with_timeout.sh 20 ./rosetta ./faults retry; ROSETTA_THRESHOLD=1 sh ../tests/with_timeout.sh 20 ./rosetta ./faults retry 2>/dev/null; echo "code $?"; }
r_fltr()  { echo "handler: signal 10 at the page yes, making it writable"; echo "repeated write: value 7";
            echo "handler: signal 10 at the page yes, making it writable"; echo "code 138"; }
t_calc()  { ./rosetta ./calculate; }
r_calc()  { ./calculate.ppc; }
t_id()    { ./rosetta ./identity; }
r_id()    { cat ../tests/identity.expected; }
t_remap() { ./rosetta ./remap; ./rosetta ./remap; ROSETTA_THRESHOLD=1 ./rosetta ./remap; }   # second run: translation cache
r_remap() { cat ../tests/remap.expected ../tests/remap.expected ../tests/remap.expected; }
t_remapc() { rm -f /tmp/rr_remap_cache; for n in 1 2 1 2; do ROSETTA_CACHE_FILE=/tmp/rr_remap_cache ./rosetta ./remap $n; done; rm -f /tmp/rr_remap_cache; }
r_remapc() { for n in 1 2 1 2; do echo "file $n: 10$n (sum 10${n}000)"; done; }
t_shf()   { ./rosetta ./shifts; ROSETTA_THRESHOLD=1 ./rosetta ./shifts; ROSETTA_INTERP=1 ./rosetta ./shifts; }
r_shf()   { ./shifts.ppc; ./shifts.ppc; ./shifts.ppc; }
t_cmpsb() { ROSETTA_THRESHOLD=1 ./rosetta ./cmpsb; }
r_cmpsb() { ROSETTA_INTERP=1 ./rosetta ./cmpsb; }
t_idb()   { ROSETTA_THRESHOLD=1 ./rosetta ./incdec8; }
r_idb()   { ROSETTA_INTERP=1 ./rosetta ./incdec8; }
same raw_arith t_raw r_raw
same "64-bit shifts, shld/shrd with flags" t_shf r_shf
same "cmpsb (translated against interpreter)" t_cmpsb r_cmpsb
same "incb/decb with flags (against interpreter)" t_idb r_idb
same "packed doubles with special values (against interpreter)" t_pdo r_pdo
same "SSE2 integer and packed floats with AltiVec (against interpreter)" t_vec r_vec
same "threads (mutex, condition, spinlock, pipe, join)" t_thr r_thr
same "loops interruptible (signal, waiting thread)" t_intr r_intr
same "Ctrl+C in a compute loop (SIGINT with handler)" t_ctrlc r_ctrlc
if [ -d $R/intel/root ]; then      # stage 4: Intel frameworks from the Combo Update
    same "CoreFoundation (Intel)" t_cf r_cf
    same "Objective-C with category" t_objc r_objc
    same "Foundation (Intel)" t_fnd r_fnd
    same "threads with Foundation (NSThread, NSOperationQueue)" t_thrns r_thrns
    same "vecLib: vDSP and vForce with doubles (against interpreted)" t_vd r_vd
    same "Calculator's engine (Calculate.framework, native replacement)" t_calc r_calc
fi
same word16 t_w16 r_w16
same "signals (handler, mask, alarm, pause, SIGPIPE)" t_sig r_sig
same "processes, kqueue, locks, semaphores, files, blocked signals, pty" t_sys r_sys
same "exceptions to the program's handlers (SIGFPE, SIGILL, SIGTRAP)" t_flt r_flt
same "exceptions, interpreter only" t_flti r_flt
same "exceptions, translated at once" t_fltj r_flt
same "memory fault handler that returns (exact in the interpreter, translated ends)" t_fltr r_fltr
same "processor identity (Intel Core Duo, SSE3)" t_id r_id
same "code replaced by remapping and writing (with translation cache)" t_remap r_remap
same "translation cache: another file at the address of inlined code" t_remapc r_remapc
same "signal without handler terminates" t_sige r_sige
same "sed (Intel)" t_sed r_sed
same fpcompare t_fpc r_fpc
same array_end t_ae r_ae
same "bench_libm (stubs)" t_t4 r_t4
same "bench_libm without stubs (against interpreter)" t_t4o r_t4o
same bench_float t_t3 r_t3
same "bench_sse2 (packed doubles)" t_t5 r_t5
same "bench_sse2 translated at once, hoisted" t_t5s r_t5
same "bench_sse2 without high halves in the FPR" t_t5o r_t5
same memtest t_mem r_mem
same str_list t_str r_str
same sse_int t_sse r_sse
same bench_control_flow t_t2 r_t2
same conditions t_cond r_cond
same bench_integer t_int r_int
same hello t_hello r_hello
same "ls -la" t_ls r_ls
same "ls -laR man1" t_lsr r_lsr
# stage 5: fonts through the ATSServer of the login session (only if someone is logged in)
if [ -d $R/intel/root ]; then
    e=$(sh ../tests/in_session.sh ../tests/fonts_check.sh | head -1)
    case "$e" in
        ok*) echo "ok      fonts: metrics, outlines, paths (ATS, against PowerPC)" ;;
        no*) echo "--      fonts: $e" ;;
        *) echo "FAILED  fonts: $e"; failed=1 ;;
    esac
fi
exit $failed
