#!/bin/sh
# in_session.sh SCRIPT [ARG...] [-- timeout] - run a shell script in the login session (Aqua: WindowServer
# reachable, otherwise not from SSH). Through the launchd of the session (LAUNCHD_SOCKET, launchctl submit); the script
# runs under sh, its output arrives here afterwards. Without root. SESSION_ENV="A=1 B=2" sets environment variables.
TIMEOUT=${TIMEOUT:-120}
SOCK=$(for s in /tmp/launchd-*/sock; do [ -O "$s" ] && echo $s && break; done)
[ -n "$SOCK" ] || { echo "no login session found"; exit 1; }
L=local.rosetta.session.$$
A=/tmp/session.$$.out
M=/tmp/session.$$.done
H=/tmp/session.$$.sh
rm -f $A $M
# wrapper: run the script, report the end, then wait (submit keeps jobs alive: do not let it restart them)
{ echo "cd $PWD"; echo "$SESSION_ENV sh $*"; echo "echo \"exit \$?\" > $M"; echo "sleep 100000"; } > $H
env LAUNCHD_SOCKET=$SOCK launchctl submit -l $L -o $A -e $A -- /bin/sh $H
i=0
while [ ! -f $M ] && [ $i -lt $TIMEOUT ]; do sleep 1; i=$((i + 1)); done
env LAUNCHD_SOCKET=$SOCK launchctl remove $L
cat $A
[ -f $M ] && cat $M || echo "timeout ($TIMEOUT s) expired"
rm -f $A $M $H
