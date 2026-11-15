#!/bin/sh
# app_survey.sh [SECONDS] APP... - (in the login session, through in_session.sh) start the i386 part of each
# application bundle under rosetta where it is installed, wait SECONDS (default 40), then report one line per program:
# windows the program has on screen, processor time, whether it is still running (or how it ended), and the first
# messages of the translator (calls that are not implemented, stub libraries reporting a missing function, crashes).
# Details in /tmp/survey/NAME.log, an image of the first window in /tmp/survey/NAME.png. Programs without an i386
# part are reported and skipped. tools/apps_i386.sh lists the candidates on this machine.
R=$(cd "$(dirname "$0")/.." && pwd)
T=40
case "$1" in [0-9]*) T=$1; shift ;; esac
O=/tmp/survey
mkdir -p $O
printf '%-28s %-7s %-9s %-12s %s\n' program windows cpu state messages
for A in "$@"; do
    A=${A%/}
    n=$(basename "$A" .app)
    s=$(echo "$n" | tr ' ' '_')
    e=$(defaults read "$A/Contents/Info" CFBundleExecutable 2>/dev/null)
    f="$A/Contents/MacOS/$e"
    if ! lipo -info "$f" 2>/dev/null | grep -q i386; then printf '%-28s no i386 part\n' "$n"; continue; fi
    rm -f $O/$s.log $O/$s.png
    c0=$(ls ~/Library/Logs/rosetta 2>/dev/null | wc -l)
    "$R/build/rosetta" "$f" > $O/$s.log 2>&1 &
    p=$!
    i=0
    while [ $i -lt $T ] && kill -0 $p 2>/dev/null; do sleep 1; i=$((i + 1)); done
    if kill -0 $p 2>/dev/null; then
        w=$("$R/tools/window_list" $p 2>/dev/null | awk '/^WINDOW/ && $8 > 40 && $9 > 40 { n++; if (!g) g = $6 " " $7 " " $8 " " $9 } END { print n + 0 "|" g }')
        cpu=$(ps -o time= -p $p | tr -d ' ')
        state=running
        g=${w#*|}
        [ -n "$g" ] && sh "$R/tests/capture_rect.sh" $g $O/$s.png > /dev/null 2>&1
        kill $p 2>/dev/null; sleep 2; kill -9 $p 2>/dev/null
        wait $p 2>/dev/null
        w=${w%%|*}
    else
        wait $p; st=$?
        w=0; cpu=-
        state="exit $st"
        [ $st -gt 128 ] && state="signal $((st - 128))"
    fi
    c1=$(ls ~/Library/Logs/rosetta 2>/dev/null | wc -l)
    [ "$c1" -gt "$c0" ] && state="$state, crash log"
    msg=$(grep -v '^$' $O/$s.log | grep -e 'rosetta' -e 'missing' -e 'not implemented' -e 'unknown' -e rror | sort | uniq -c | sort -rn | head -3 | sed 's/^ *//' | tr '\n' ';' | cut -c1-150)
    printf '%-28s %-7s %-9s %-12s %s\n' "$n" "$w" "$cpu" "$state" "$msg"
done
