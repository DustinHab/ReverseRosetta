#!/bin/sh
# app_state.sh PROGRAM_PATH [SECONDS] [TIMES] - (in the login session) run a program with ROSETTA_STATE, after
# SECONDS query the guest state TIMES times at 2 s intervals (SIGUSR2), chains symbolic, then stop it
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
ROSETTA_STATE=1 ./rosetta "$1" > /tmp/az_out.txt 2>&1 &
p=$!
sleep ${2:-40}
i=0
while [ $i -lt ${3:-3} ]; do kill -USR2 $p; sleep 2; i=$((i + 1)); done
kill -9 $p 2>/dev/null; wait $p 2>/dev/null
grep -A1 "^STATE" /tmp/az_out.txt > /tmp/az.txt
cat /tmp/az.txt
k=$(grep "chain:" /tmp/az.txt | tail -1 | sed 's/.*chain://')
e=$(grep "^STATE" /tmp/az.txt | tail -1 | sed 's/^STATE eip \([0-9a-f]*\).*/\1/')
sh ../tests/guest_sym.sh $e $k 2>&1 | grep -v "^    in" | head -26
