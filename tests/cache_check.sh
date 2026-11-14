#!/bin/sh
# cache_check.sh NAME [ARGS] - translation cache: delete the cache, run twice (create, use), compare the output,
# statistics of the second run. NAME in the build directory.
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
N=$1; shift
rm -f ~/Library/Caches/rosetta/cache-*
ROSETTA_STATS=1 ./rosetta ./$N "$@" > /tmp/vr1.txt 2> /tmp/vr1e.txt; s1=$?
ls -l ~/Library/Caches/rosetta/ | grep cache- | awk '{print "translation cache " $5 " bytes"}'
ROSETTA_CACHE_SHOW=1 ROSETTA_STATS=1 ./rosetta ./$N "$@" > /tmp/vr2.txt 2> /tmp/vr2e.txt; s2=$?
echo "Status $s1 / $s2"
if cmp -s /tmp/vr1.txt /tmp/vr2.txt; then echo "Output same"; else echo "OUTPUT DIFFERENT"; diff /tmp/vr1.txt /tmp/vr2.txt | head -10; fi
grep -E "translation cache|blocks with|rosetta: 0\.|translation took" /tmp/vr2e.txt | sed 's/, %lu.*//' | cut -c1-200
grep -iE "fault|signal" /tmp/vr2e.txt | head -5
