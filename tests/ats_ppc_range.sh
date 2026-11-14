#!/bin/sh
# ats_ppc_range.sh FROM TO [server|empty] - address range (hex, 8 digits) from the PowerPC ATS disassembly
# (compared as strings: awk would otherwise read "000059e4" as the number 59e4)
C=/tmp/ats_ppc_$3.txt
awk -v a="$1" -v b="$2" '{ x = $1 "" } length(x) == 8 && x >= a "" && x <= b ""' $C
