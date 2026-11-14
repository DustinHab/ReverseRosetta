#!/bin/sh
# stats_short.sh PROG [ARGS] - the most important counters from ROSETTA_STATS (time, translation, cold runs, system calls)
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
ROSETTA_STATS=1 ./rosetta "$@" 2>&1 >/dev/null | grep -E '^rosetta: [0-9.]+ s|translation took|blocks with|threshold|system calls|paths redirected|thunk_find' | cut -c1-220
