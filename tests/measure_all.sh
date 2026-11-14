#!/bin/sh
# measure_all.sh - the benchmark loads one after another
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/tests || exit 1
sh measure.sh bench_integer 1 31
sh measure.sh bench_control_flow 1 11
sh measure.sh bench_float 1 21
sh measure.sh bench_libm 1 41
sh measure.sh bench_calls 1 11
sh measure.sh bench_sse2 1 101
