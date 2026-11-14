#!/bin/sh
# pd_check.sh [ROUNDS] - bench_sse2 (packed doubles) in all translator modes against native and the interpreter
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
ulimit -t 300
r=${1:-20}
./bench_sse2.ppc $r > /tmp/pd_n.txt
failed=0
probe() {   # name, env...
    name=$1; shift
    env "$@" ./rosetta ./bench_sse2 $r > /tmp/pd_x.txt 2>/tmp/pd_e.txt
    if cmp -s /tmp/pd_x.txt /tmp/pd_n.txt; then echo "ok      $name"; else echo "FAILED  $name"; diff /tmp/pd_x.txt /tmp/pd_n.txt | head -8; tail -3 /tmp/pd_e.txt; failed=1; fi
}
probe translated ROSETTA_X=1
probe "threshold 1" ROSETTA_THRESHOLD=1
probe "without high-half FPR" ROSETTA_NO_XFH=1 ROSETTA_THRESHOLD=1
probe "without packed" ROSETTA_NO_PD=1
probe "without constants" ROSETTA_NO_CONST=1 ROSETTA_THRESHOLD=1
probe "with hoisting" ROSETTA_FPAHEAD=1 ROSETTA_THRESHOLD=1
probe interpreter ROSETTA_INTERP=1
exit $failed
