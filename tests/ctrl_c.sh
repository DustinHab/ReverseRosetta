#!/bin/sh
# ctrl_c.sh [COMMAND...] - SIGINT after 1 s to a program in a compute loop: output, status, reaction time
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
[ $# -eq 0 ] && set -- ./rosetta ./ctrl_c
"$@" > /tmp/sc.txt 2>&1 &
p=$!
sleep 1
t0=$(perl -MTime::HiRes=time -e 'printf "%.0f", time * 1000')
kill -INT $p
wait $p
s=$?
t1=$(perl -MTime::HiRes=time -e 'printf "%.0f", time * 1000')
echo "Status $s after $((t1 - t0)) ms"
cat /tmp/sc.txt
