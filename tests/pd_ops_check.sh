#!/bin/sh
# pd_ops_check.sh - pd_ops (packed doubles with special values) in all translator modes against the interpreter
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
ulimit -t 120
ROSETTA_INTERP=1 ./rosetta ./pd_ops > /tmp/po_i.txt
failed=0
for v in "ROSETTA_X=1" "ROSETTA_THRESHOLD=1" "ROSETTA_THRESHOLD=1 ROSETTA_NO_XFH=1" "ROSETTA_THRESHOLD=1 ROSETTA_FPAHEAD=1" \
         "ROSETTA_THRESHOLD=1 ROSETTA_NO_CONST=1" "ROSETTA_NO_PD=1"; do
    env $v ./rosetta ./pd_ops > /tmp/po_x.txt 2>&1
    if cmp -s /tmp/po_x.txt /tmp/po_i.txt; then echo "ok      $v"; else echo "FAILED  $v"; diff /tmp/po_i.txt /tmp/po_x.txt | head -10; failed=1; fi
done
exit $failed
