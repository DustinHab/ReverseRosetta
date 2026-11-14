#!/bin/sh
# cache_startup.sh - startup times with and without the translation cache (fastest of 5, cache created beforehand)
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
z() { perl -MTime::HiRes=time -e 'my $m = 1e9; for (1..5) { my $t = time; system("@ARGV > /dev/null 2>&1"); $t = time - $t; $m = $t if $t < $m } printf "%.0f", $m * 1000' "$@"; }
for p in hello cf foundation objc_plain; do
  ./rosetta ./$p > /dev/null 2>&1
  echo "$p: native $(z ./$p.ppc) ms, without cache $(ROSETTA_NO_CACHE=1 z ./rosetta ./$p) ms, with cache $(z ./rosetta ./$p) ms"
done
./rosetta /bin/ls -la /usr/lib > /dev/null 2>&1
echo "ls -la: native $(z /bin/ls -la /usr/lib) ms, without cache $(ROSETTA_NO_CACHE=1 z ./rosetta /bin/ls -la /usr/lib) ms, with cache $(z ./rosetta /bin/ls -la /usr/lib) ms"
