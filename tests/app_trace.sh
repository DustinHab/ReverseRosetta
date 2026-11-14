#!/bin/sh
# app_trace.sh NAME [SECONDS] - (in the login session) Intel program with the system call log, stop it after SECONDS
R=$(cd "$(dirname "$0")/.." && pwd)
P=$R/intel/apps/$1.app
b=$(defaults read "$P/Contents/Info" CFBundleExecutable)
cd $R/build || exit 1
ROSETTA_LOG=1 ./rosetta "$P/Contents/MacOS/$b" > /tmp/gpr_out.txt 2> /tmp/gpr_log.txt &
p=$!
sleep ${2:-30}
kill -9 $p 2>/dev/null
wait $p 2>/dev/null
echo "$(wc -l < /tmp/gpr_log.txt) log lines"
