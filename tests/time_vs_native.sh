#!/bin/sh
# time_vs_native.sh NAME [ARGS] - run time native (build/NAME.ppc) against rosetta (build/NAME), fastest of 3
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
N=$1; shift
z() { perl -MTime::HiRes=time -e 'my $m = 1e9; for (1..3) { my $t = time; system("@ARGV > /dev/null 2>&1"); $t = time - $t; $m = $t if $t < $m } printf "%.0f", $m * 1000' "$@"; }
echo "$N: native $(z ./$N.ppc "$@") ms, rosetta $(z ./rosetta ./$N "$@") ms"
