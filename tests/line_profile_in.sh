#!/bin/sh
# line_profile_in.sh FUNCTION [PROG] [RUNS] [N] - line profile (line_profile.sh) only for hits in FUNCTION (with inlined code)
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R || exit 1
F=$1; P=${2:-hello}; L=${3:-30}; N=${4:-40}
sh tests/line_profile.sh "$P" "$L" 5000 > /tmp/zl_all.txt 2>&1
tail -1 /tmp/zl_all.txt
grep " $F\$" /tmp/zl_all.txt | head -$N
grep " $F\$" /tmp/zl_all.txt | awk '{ s += $1 } END { printf "sum in %s: %.1f%%\n", "'"$F"'", s }'
