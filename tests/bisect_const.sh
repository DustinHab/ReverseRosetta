#!/bin/sh
# bisect_const.sh LO HI PROG... - bisect the block range in which constant prediction changes the result (against
# the run without prediction)
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
ulimit -t 120
lo=$((0x$1)); hi=$((0x$2)); shift 2
good=$(ROSETTA_NO_CACHE=1 ROSETTA_CONST=0-0 ./rosetta "$@" 2>&1 | md5)    # without prediction as reference
differs() { k=$(printf %x-%x $1 $2); shift 2; [ "$(ROSETTA_NO_CACHE=1 ROSETTA_CONST=$k ./rosetta "$@" 2>&1 | md5)" != "$good" ]; }
differs $lo $hi "$@" || { echo "range without error"; exit 0; }
while [ $((hi - lo)) -gt 0 ]; do
    mi=$(( (lo + hi) / 2 ))
    if differs $lo $mi "$@"; then hi=$mi
    elif differs $((mi + 1)) $hi "$@"; then lo=$((mi + 1))
    else printf "only together: %08x-%08x\n" $lo $hi; exit 0; fi
done
printf "Block: %08x\n" $lo
