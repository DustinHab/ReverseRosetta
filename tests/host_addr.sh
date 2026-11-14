#!/bin/sh
# host_addr.sh ADDR... - translator function (also static ones) for host addresses
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
nm -n rosetta | grep -i ' t ' > /tmp/adr_sym.txt
for a in "$@"; do
  awk -v z="$a" 'BEGIN { z = tolower(z) } { if ($1 <= z) l = $3 } END { print z, l }' /tmp/adr_sym.txt
done
