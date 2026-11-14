#!/bin/sh
# bisect_blocks.sh LO HI PROG... - bisect the block range whose translation changes the result compared with the
# interpreter (ROSETTA_ONLY: translate only there)
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
ulimit -t 300
lo=$((0x$1)); hi=$((0x$2)); shift 2
good=$(ROSETTA_INTERP=1 ./rosetta "$@" 2>&1 | md5)
differs() { k=$(printf %x-%x $1 $2); shift 2; [ "$(ROSETTA_NO_CACHE=1 ROSETTA_THRESHOLD=2 ROSETTA_ONLY=$k ./rosetta "$@" 2>&1 | md5)" != "$good" ]; }
differs $lo $hi "$@" || { echo "range without error"; exit 0; }
while [ $((hi - lo)) -gt 0 ]; do
    mi=$(( (lo + hi) / 2 ))
    if differs $lo $mi "$@"; then hi=$mi
    elif differs $((mi + 1)) $hi "$@"; then lo=$((mi + 1))
    else printf "only together: %08x-%08x\n" $lo $hi; exit 0; fi
done
printf "Block: %08x\n" $lo
