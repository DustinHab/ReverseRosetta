#!/bin/sh
# mips_ab.sh LOAD ROUNDS "ENV_A" "ENV_B" [N] - benchmark load alternately with A and B (N times each), MIPS lines
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/tests || exit 1
L=$1; R=$2; A=$3; B=$4; N=${5:-2}
i=0
while [ $i -lt $N ]; do
  for v in "$A" "$B"; do
    echo "$v: $(env $v sh measure.sh $L 1 $R | tail -1 | sed 's/.*per round //')"
  done
  i=$((i + 1))
done
