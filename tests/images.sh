#!/bin/sh
# images.sh PROG [ARGS] - loaded Intel images at the end of the run: location, requested location, whether moved
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
ROSETTA_STATS=1 ROSETTA_IMAGES=1 ./rosetta "$@" 2>&1 >/dev/null | grep 'rosetta: image ' | sed 's/^rosetta: image //' | cut -c1-160
