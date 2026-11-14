#!/bin/sh
# host_where.sh ADDRESS... - machine code in rosetta around an address, with the last label before it
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
otool -tv rosetta > /tmp/wo_dis.txt
for a in "$@"; do
  echo "=== $a"
  awk -v z="$a" '
    /^[_a-zA-Z].*:$/ { label = $0; next }
    { if ($1 == z) { print label; for (i = 0; i < n; i++) print buf[(k + i) % 12]; print ">>> " $0; rest = 6; next }
      if (rest > 0) { print; rest--; if (rest == 0) exit }
      buf[k % 12] = $0; k++; if (n < 12) n++ }
  ' /tmp/wo_dis.txt
done
