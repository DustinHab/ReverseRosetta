#!/bin/sh
# bench_control_flow_parts.sh - build the four parts of bench_control_flow separately (i386 and native) and measure them reliably
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R || exit 1
for n in 1 2 3 4; do
  gcc-4.2 -arch i386 -O2 -msse2 -DPART=$n -o build/t2_$n tests/bench_control_flow_part.c
  gcc-4.2 -arch ppc -mcpu=7450 -O2 -mno-fused-madd -DPART=$n -o build/t2_$n.ppc tests/bench_control_flow_part.c
done
cd tests
sh measure.sh t2_1 1 11
sh measure.sh t2_2 1 11
sh measure.sh t2_3 1 11
sh measure.sh t2_4 1 11
