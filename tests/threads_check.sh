#!/bin/sh
# threads_check.sh [PROG] - thread test native against rosetta (translated, threshold 1, interpreter), with a wall-clock timeout
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
P=${1:-threads}
./$P.ppc > /tmp/fd_n.txt 2>&1
for v in ROSETTA_X=1 ROSETTA_THRESHOLD=1 ROSETTA_INTERP=1; do
  env $v sh ../tests/with_timeout.sh 60 ./rosetta ./$P > /tmp/fd_r.txt 2>&1; s=$?
  if cmp -s /tmp/fd_n.txt /tmp/fd_r.txt; then echo "$v: same (status $s)"
  else echo "$v: DIFFERENT (status $s)"; diff /tmp/fd_n.txt /tmp/fd_r.txt | head -12; fi
done
cat /tmp/fd_n.txt
