#!/bin/sh
# run_all.sh [time] - test suite (twice: create the cache, use it), framework programs with the cache, with "time" startup times
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R || exit 1
for i in 1 2; do
  sh tests/run_tests.sh > /tmp/al_p.txt 2>&1
  echo "test suite: $(grep -c '^ok' /tmp/al_p.txt) ok, $(grep -c '^FAILED' /tmp/al_p.txt) failed"
  grep -A5 '^FAILED' /tmp/al_p.txt | head -20
done
sh tests/cache_check_all.sh short > /tmp/al_v.txt 2>&1
echo "framework programs: $(grep -c 'Output same' /tmp/al_v.txt) same, $(grep -c 'OUTPUT DIFFERENT' /tmp/al_v.txt) different"
grep -E '^Status' /tmp/al_v.txt | sort | uniq -c
[ "$1" = time ] && sh tests/cache_startup.sh
exit 0
