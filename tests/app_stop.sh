#!/bin/sh
# app_stop.sh NAME - stop a program started with app_start.sh (remove the job) and show the output
N=$1
S=$(echo "$N" | tr ' ' '_')
SOCK=$(for s in /tmp/launchd-*/sock; do [ -O "$s" ] && echo $s && break; done)
env LAUNCHD_SOCKET=$SOCK launchctl remove local.rosetta.app.$S && echo "stopped: $N"
rm -f /tmp/app_start.$S.sh
tail -20 /tmp/app_$S.log 2>/dev/null
