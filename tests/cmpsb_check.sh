#!/bin/sh
# cmpsb_check.sh - cmpsb test: translated (threshold 1, without cache) against the interpreter and against the old handling
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
ROSETTA_NO_CACHE=1 ROSETTA_INTERP=1 ./rosetta ./cmpsb > /tmp/cm_i.txt 2>&1
ROSETTA_NO_CACHE=1 ROSETTA_THRESHOLD=1 ./rosetta ./cmpsb > /tmp/cm_u.txt 2>&1
ROSETTA_NO_CACHE=1 ROSETTA_THRESHOLD=1 ROSETTA_NO_CMPSB=1 ./rosetta ./cmpsb > /tmp/cm_a.txt 2>&1
head -12 /tmp/cm_i.txt
if cmp -s /tmp/cm_i.txt /tmp/cm_u.txt; then echo "translated = interpreter"; else echo "DIFFERENT (translated)"; diff /tmp/cm_i.txt /tmp/cm_u.txt | head; fi
if cmp -s /tmp/cm_i.txt /tmp/cm_a.txt; then echo "old = interpreter"; else echo "DIFFERENT (old)"; diff /tmp/cm_i.txt /tmp/cm_a.txt | head; fi
