#!/bin/sh
# compare_env.sh PROG K G VAR=VALUE... - reliable measurement alternately with and without the settings (twice)
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/tests || exit 1
P=$1; K=$2; G=$3; shift 3
for i in 1 2; do
  printf "without: "; sh measure.sh $P $K $G
  printf "with:    "; env "$@" sh measure.sh $P $K $G
done
