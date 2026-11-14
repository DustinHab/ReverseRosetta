#!/bin/sh
# fonts_trace.sh - (in the login session) fonts under rosetta with the ATS log to /tmp/app_out.txt
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
ROSETTA_ATS_LOG=1 ./rosetta ./fonts > /tmp/app_out.txt 2>&1
grep -c "^\[ats to" /tmp/app_out.txt
