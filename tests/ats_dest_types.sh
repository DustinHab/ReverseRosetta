#!/bin/sh
# ats_dest_types.sh - OFA requests with a target in shared memory: mtyp, rtyp, glyph list, dest length (frequency)
R=$(cd "$(dirname "$0")/.." && pwd)
sh $R/tests/ats_dest.sh 400 | grep "ool 0" | awk '{
    s = ""
    for (i = 1; i <= NF; i++) {
        if ($i == "6d747970" || $i == "72747970") s = s $i "=" $(i + 2) " "
        if ($i == "67617279") s = s "gary=" $(i + 1) " "
        if ($i == "64657374") s = s "dest=" $(i + 2) " "
    }
    print s }' | sort | uniq -c
