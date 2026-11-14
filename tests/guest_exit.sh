#!/bin/sh
# guest_exit.sh PROG [ARG] - find the caller of exit(): interpreter with a halt at the start of exit (Intel libSystem), chain symbolic
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
L=$R/intel/root/usr/lib/libSystem.B.dylib
[ -f $L ] || L=/usr/lib/libSystem.B.dylib
a=$(nm -arch i386 $L | awk '$3 == "_exit" {print $1}')
echo "exit at $a ($L)"
ROSETTA_INTERP=1 ROSETTA_HALT=$a ./rosetta ./$1 $2 2>&1 | grep -A1 "^HALT" | head -4 > /tmp/ge.txt
cat /tmp/ge.txt
k=$(grep "chain:" /tmp/ge.txt | head -1 | sed 's/.*chain://')
s=$(grep "^HALT" /tmp/ge.txt | head -1 | sed 's/.*\[esp\] \([0-9a-f]*\).*/\1/')
sh ../tests/guest_sym.sh $s $k
