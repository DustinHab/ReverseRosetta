#!/bin/sh
# translate_time.sh [PROG] - total startup time and the share of translation (fastest of 5 runs)
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
P=${1:-hello}
i=0; best=999; bestu=999
while [ $i -lt 5 ]; do
  u=$(ROSETTA_STATS=1 ./rosetta ./$P 2>&1 >/dev/null | sed -n 's/^rosetta: translation took \([0-9.]*\) ms/\1/p')
  t=$(perl -MTime::HiRes=time -e '$t = time; system("./rosetta ./'$P' > /dev/null 2>&1"); printf "%.1f", (time - $t) * 1000')
  best=$(perl -e "print $t < $best ? $t : $best"); bestu=$(perl -e "print $u < $bestu ? $u : $bestu")
  i=$((i + 1))
done
echo "$P: start $best ms, of which translation ~$bestu ms (measured with ROSETTA_STATS)"
