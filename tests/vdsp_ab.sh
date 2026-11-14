#!/bin/sh
# vdsp_ab.sh "ENV..."... - vdsp_d with different switches against the interpreter: only differing lines
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
[ -f /tmp/vd_i.txt ] || ROSETTA_INTERP=1 ./rosetta ./vdsp_d > /tmp/vd_i.txt 2>&1
for v in "$@"; do
    env $v ./rosetta ./vdsp_d > /tmp/vd_x.txt 2>&1
    echo "== $v: $(diff /tmp/vd_i.txt /tmp/vd_x.txt | grep '^>' | grep -v total | awk '{print $2, $3}' | tr '\n' ' ')"
done
