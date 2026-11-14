#!/bin/sh
# app_start.sh NAME [ARGS...] - start an Intel program (intel/apps/NAME.app) under rosetta in the login session and
# keep it running (for interactive use); output in /tmp/app_NAME.log. Stop: app_stop.sh NAME (or in the program itself).
# ARGS go to the program, e.g. -AppleLanguages "(en)" for the English user interface.
# APP_ENV="A=1 B=2" sets environment variables. The job waits after the end (submit restarts it otherwise).
R=$(cd "$(dirname "$0")/.." && pwd)
N=$1; shift
A=""
for x in "$@"; do A="$A '$x'"; done
P=$R/intel/apps/$N.app
b=$(defaults read "$P/Contents/Info" CFBundleExecutable) || exit 1
SOCK=$(for s in /tmp/launchd-*/sock; do [ -O "$s" ] && echo $s && break; done)
[ -n "$SOCK" ] || { echo "no login session found"; exit 1; }
L=local.rosetta.app.$N
H=/tmp/app_start.$N.sh
env LAUNCHD_SOCKET=$SOCK launchctl remove $L 2>/dev/null
{ echo "cd $R/build"; echo "$APP_ENV ./rosetta \"$P/Contents/MacOS/$b\"$A > /tmp/app_$N.log 2>&1"
  echo "echo \"exit \$?\" >> /tmp/app_$N.log"; echo "sleep 100000"; } > $H
env LAUNCHD_SOCKET=$SOCK launchctl submit -l $L -- /bin/sh $H
sleep 2
ps -axo pid,command | grep "rosetta $P" | grep -v grep
