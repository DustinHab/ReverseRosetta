#!/bin/sh
# guest_break.sh SYMBOL PROGRAM [SECONDS] - interpreter with a halt at a function of the Intel libSystem; first hits with
# symbolic call chain (for in_session.sh with graphical programs)
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
L=$R/intel/root/usr/lib/libSystem.B.dylib
a=$(nm -arch i386 $L | awk -v s="$1" '$3 == s {print $1}' | head -1)
echo "$1 at $a"
ROSETTA_INTERP=1 ROSETTA_HALT=$a ./rosetta "$2" > /dev/null 2> /tmp/gh_out.txt &
p=$!
sleep ${3:-60}
kill -9 $p 2>/dev/null; wait $p 2>/dev/null
grep -A1 "^HALT" /tmp/gh_out.txt | tail -4 > /tmp/gh.txt
cat /tmp/gh.txt
k=$(grep "chain:" /tmp/gh.txt | tail -1 | sed 's/.*chain://')
s=$(grep "^HALT" /tmp/gh.txt | tail -1 | sed 's/.*\[esp\] \([0-9a-f]*\).*/\1/')
sh ../tests/guest_sym.sh $s $k 2>&1 | grep -v "^    in" | head -20
