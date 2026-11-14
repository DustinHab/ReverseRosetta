#!/bin/sh
# dyld_stat.sh PROG - statistics of the i386 dyld under rosetta (DYLD_PRINT_STATISTICS)
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
for i in 1 2; do ./rosetta ./$1 > /dev/null 2>&1; done
DYLD_PRINT_STATISTICS=1 ./rosetta ./$1 2>&1 > /dev/null | grep -v "^rosetta"
