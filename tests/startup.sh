#!/bin/sh
# startup.sh [PROG...] - startup time (0 rounds, or hello), fastest of 5
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
fastest() {
    perl -MTime::HiRes=time -e 'my $min = 1e9; for (1 .. 5) { my $t = time; system("@ARGV > /dev/null 2>&1"); $t = time - $t; $min = $t if $t < $min; } printf "%.3f s\n", $min;' "$@"
}
printf "%-12s " "hello"; fastest ./rosetta ./hello
for p in "$@"; do printf "%-12s " "$p 0"; fastest ./rosetta ./$p 0; done
