#!/bin/sh
# tiers.sh ["PROG ARGS"] - run time and translation time for different thresholds of tiered translation
# (fastest of 5 runs per threshold), plus the counters from ROSETTA_STATS. Thresholds in $S (default 1 2 3 5 10 20)
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
P=${1:-./hello}
for s in ${S:-1 2 3 5 10 20}; do
  best=999999
  i=0
  while [ $i -lt ${N:-5} ]; do
    t=$(ROSETTA_THRESHOLD=$s perl -MTime::HiRes=time -e '$t = time; system("./rosetta '"$P"' > /dev/null 2>&1"); printf "%.1f", (time - $t) * 1000')
    best=$(perl -e "print $t < $best ? $t : $best")
    i=$((i + 1))
  done
  info=$(ROSETTA_THRESHOLD=$s ROSETTA_STATS=1 ./rosetta $P 2>&1 >/dev/null | perl -ne '
    print "$1 blocks/$2 instructions translated, " if /(\d+) blocks with (\d+) instructions translated/;
    print "translation $1 ms, " if /translation took ([0-9.]+) ms/;
    print "$1 cold runs ($2 instructions)" if /(\d+) cold runs interpreted \((\d+) instructions\)/;')
  echo "threshold $s: $best ms; $info"
done
