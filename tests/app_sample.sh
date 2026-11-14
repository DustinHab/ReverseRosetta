#!/bin/sh
# app_sample.sh PROGRAM_PATH [SECONDS] - (in the login session) program under rosetta, after SECONDS sample it for 3 s
# with sample (all host threads), report to /tmp/app_sample.txt, then stop it
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
./rosetta "$1" > /tmp/as_out.txt 2>&1 &
p=$!
sleep ${2:-40}
sample $p 3 -file /tmp/app_sample.txt > /dev/null 2>&1
kill -9 $p 2>/dev/null; wait $p 2>/dev/null
grep -c . /tmp/app_sample.txt
