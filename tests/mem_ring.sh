#!/bin/sh
# mem_ring.sh [FILE] - fault line and ring buffer of memory calls from the output of a run, chains symbolic
R=$(cd "$(dirname "$0")/.." && pwd)
f=${1:-/tmp/as_out.txt}
grep -E '^rosetta: fault|^  [a-zA-Z] ' "$f" | head -12
grep -E '^  [a-zA-Z] .*\*$' "$f" | head -3 | while read kind range eip caller k1 k2 k3 rest; do
    echo "== $kind $range"
    sh $R/tests/guest_sym.sh $k1 $k2 $k3 2>&1 | grep -v '^    in'
done
