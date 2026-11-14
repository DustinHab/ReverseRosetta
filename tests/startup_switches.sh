#!/bin/sh
# startup_switches.sh ["PROG ARGS"] [N] - startup time with one optimization switched off at a time against all on (minimum of N, alternating)
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R || exit 1
P=${1:-./hello}; N=${2:-12}
for s in NO_SLOTS NO_PREFETCH NO_CONST NO_EMBED NO_SW NO_FW NO_VARIANTS NO_FUSION; do
  sh tests/ab.sh X=1 "ROSETTA_$s=1" "$P" "$N" | awk -v s=$s '/^A:/ { a = $3 } /^B:/ { b = $3 } END { printf "%-16s all on %s ms, without %s ms, difference %+.1f ms\n", s, a, b, b - a }'
done
