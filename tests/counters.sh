#!/bin/sh
# counters.sh PROG ARG PATTERN - translator counters (ROSETTA_STATS) filtered by pattern
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
ROSETTA_STATS=1 ./rosetta ./$1 $2 2>&1 >/dev/null | tr ',' '\n' | grep -i "$3"
