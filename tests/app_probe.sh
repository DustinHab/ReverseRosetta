#!/bin/sh
# app_probe.sh NAME [SECONDS] - start an Intel program (intel/apps/NAME.app) under rosetta, stop it after SECONDS;
# output and new system.log lines; with X Y W H also the image of this rectangle (only the program window).
# For in_session.sh.
R=$(cd "$(dirname "$0")/.." && pwd)
P=$R/intel/apps/$1.app
b=$(defaults read "$P/Contents/Info" CFBundleExecutable)
n=$(wc -l < /var/log/system.log)
cd $R/build || exit 1
env ${APP_ENV:-ROSETTA_X=1} ./rosetta "$P/Contents/MacOS/$b" > /tmp/app_out.txt 2>&1 &
p=$!
sleep ${2:-30}
[ -n "$6" ] && ../tools/screen_grab $3 $4 $5 $6 /tmp/app_$1.png | tail -1
../tools/window_list $p
ps -o pid,stat,time -p $p | tail -1
kill $p 2>/dev/null; sleep 1; kill -9 $p 2>/dev/null
wait $p 2>/dev/null
cat /tmp/app_out.txt | head -30
echo "--- system.log:"
tail -n +$((n + 1)) /var/log/system.log | grep -v local.rosetta | cut -c1-220 | tail -15
