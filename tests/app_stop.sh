#!/bin/sh
# app_stop.sh NAME - stop a program started with app_start.sh (remove the job) and show the output
N=$1
SOCK=$(for s in /tmp/launchd-*/sock; do [ -O "$s" ] && echo $s && break; done)
env LAUNCHD_SOCKET=$SOCK launchctl remove local.rosetta.app.$N && echo "stopped: $N"
rm -f /tmp/app_start.$N.sh
tail -20 /tmp/app_$N.log 2>/dev/null
