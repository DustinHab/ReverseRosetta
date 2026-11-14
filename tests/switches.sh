#!/bin/sh
# switches.sh PROG [ARGS] - result against native with one part of the translator switched off at a time
# (narrowing down an error; further environment such as ROSETTA_THRESHOLD is passed through)
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
P=$1; shift
./$P.ppc "$@" > /tmp/sw_n.txt 2>&1
for s in "" NO_WRITEBACK NO_SLOTS NO_EMBED NO_VARIANTS NO_SW NO_FW NO_FW_STACK NO_CONST \
         NO_PRECOMPUTE NO_PIC NO_STUB_DIRECT NO_FUSION NO_INNER NO_CLOSE NO_PREFETCH NO_CHAIN \
         NO_XHOST NO_NATIVE NO_BUDGET NO_EXACT_SZ NO_PAGEEXACT CAREFUL; do
  if [ -n "$s" ]; then env ROSETTA_$s=1 ./rosetta ./$P "$@" > /tmp/sw_r.txt 2>&1
  else ./rosetta ./$P "$@" > /tmp/sw_r.txt 2>&1; fi
  cmp -s /tmp/sw_n.txt /tmp/sw_r.txt && echo "${s:-all on}: same" || echo "${s:-all on}: DIFFERENT $(head -c 60 /tmp/sw_r.txt)"
done
