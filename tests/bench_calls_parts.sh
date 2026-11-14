#!/bin/sh
# bench_calls_parts.sh - build the four parts of bench_calls separately and measure them reliably
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R || exit 1
for n in 1 2 3 4; do
  gcc-4.2 -arch i386 -O2 -msse2 -DPART=$n -o build/a_$n tests/bench_calls_part.c
  gcc-4.2 -arch ppc -mcpu=7450 -O2 -mno-fused-madd -DPART=$n -o build/a_$n.ppc tests/bench_calls_part.c
done
cd tests
for n in 1 2 3 4; do sh measure.sh a_$n 1 21 2>&1 | grep -v ulimit; done
