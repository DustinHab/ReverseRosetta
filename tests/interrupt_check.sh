#!/bin/sh
# interrupt_check.sh - counter-check without the watchdog (must hang) and counters with the watchdog
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
echo "== without watchdog (timeout 10 s)"
ROSETTA_NO_WATCHDOG=1 sh ../tests/with_timeout.sh 10 ./rosetta ./interrupt; echo "Status $?"
echo "== with watchdog"
sh ../tests/walltime.sh ./rosetta ./interrupt
sh ../tests/walltime.sh ./interrupt.ppc
ROSETTA_STATS=1 ./rosetta ./interrupt 2>&1 > /dev/null | grep -E "watchdog|guard|budget|interrupted" | cut -c1-200
