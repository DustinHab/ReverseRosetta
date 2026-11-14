#!/bin/sh
# base_cost.sh - base cost: native start of hello, rosetta without a guest (usage message only), rosetta with hello
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
z() { perl -MTime::HiRes=time -e 'my $m = 1e9; for (1..15) { my $t = time; system("@ARGV > /dev/null 2>&1"); $t = time - $t; $m = $t if $t < $m } printf "%.1f ms", $m * 1000' "$@"; }
echo "native hello.ppc:      $(z ./hello.ppc)"
echo "rosetta without guest: $(z ./rosetta)"
echo "/usr/bin/true:         $(z /usr/bin/true)"
echo "rosetta hello:         $(z ./rosetta ./hello)"
echo "rosetta raw (static):  $(z ./rosetta ./raw)"
