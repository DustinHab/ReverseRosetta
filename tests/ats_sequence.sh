#!/bin/sh
# ats_sequence.sh [N] - order of the ATS messages in the last run: request type -> reply status
awk '/^\[ats to\]/ { t = $4 } /^\[ats from\]/ { printf "%s->%s ", t, $4; if (++k % 12 == 0) print "" }' /tmp/app_out.txt | head -${1:-6}
echo
echo "--- first OFAStrike request and reply"
awk '/^\[ats to\] id 32 /{ p = 1 } p && /^\[ats/ { print substr($0, 1, 900); if (++k >= 2) exit }' /tmp/app_out.txt
