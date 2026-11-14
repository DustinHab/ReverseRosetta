#!/bin/sh
# ats_dest.sh [N] - the first N OFA requests with 'dest' (target in shared memory) with their reply, in server order
awk -v n="${1:-2}" '
  /^\[ats to\]/ { req = substr($0, 1, 400); ool = ""; next }
  /^  \[ool/ && req != "" { ool = ool "\n" substr($0, 1, 1400); next }
  /^\[ats from\]/ { if (req ~ / id 32 / && ool ~ /64657374/ && k < n) { print "=="; print req ool; print substr($0, 1, 500); k++ } req = "" }
' /tmp/app_out.txt
