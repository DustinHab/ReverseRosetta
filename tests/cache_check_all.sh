#!/bin/sh
# cache_check_all.sh [short] - cache_check.sh for the framework and special programs (short: only status, output, cache)
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R || exit 1
for p in foundation cf objc_plain signals signal_exit word16 long80; do
  echo "### $p"
  if [ "$1" = short ]; then sh tests/cache_check.sh $p 2>&1 | grep -E "Status|Output|OUTPUT|translation cache [0-9]" | cut -c1-120
  else sh tests/cache_check.sh $p 2>&1; fi
done
