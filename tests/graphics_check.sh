#!/bin/sh
# graphics_check.sh [native|rosetta] [PROGRAM] - (in the login session, through in_session.sh) start graphics1/graphics2, read the window, stop
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
P=${2:-graphics1}
if [ "$1" = native ]; then ./$P.ppc 20 > /tmp/gp_out.txt 2>&1 &
else ROSETTA_NO_CACHE=1 ./rosetta ./$P 20 > /tmp/gp_out.txt 2>&1 & fi
p=$!
i=0
while ! grep -q WINDOW /tmp/gp_out.txt && kill -0 $p 2>/dev/null && [ $i -lt 60 ]; do sleep 1; i=$((i + 1)); done
cat /tmp/gp_out.txt
w=$(sed -n 's/^WINDOW //p' /tmp/gp_out.txt)
[ -n "$w" ] && { sleep 1; ../tools/screen_grab 120 120 240 160 /tmp/window_${1:-rosetta}.png; }
kill $p 2>/dev/null
wait $p 2>/dev/null
