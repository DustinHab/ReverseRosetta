#!/bin/sh
# ats_trace.sh [NAME] [SECONDS] - start an Intel program (default: Calculator) with the ATS log in the login session;
# then: ATS messages from rosetta, request types, errors, window positions
R=$(cd "$(dirname "$0")/.." && pwd)
N=${1:-Calculator}
cd $R/tests || exit 1
ls $R/intel/apps/
SESSION_ENV="APP_ENV=ROSETTA_ATS_LOG=1" TIMEOUT=$((${2:-40} + 30)) sh in_session.sh app_probe.sh $N ${2:-40} | grep -v "^\[ats"
echo "--- rosetta messages"
grep -E "^rosetta" /tmp/app_out.txt | sort | uniq -c | head -30
echo "--- ATS requests by type"
grep "^\[ats to\]" /tmp/app_out.txt | awk '{ print $4 }' | sort -n | uniq -c
echo "--- reply status"
grep "^\[ats from\]" /tmp/app_out.txt | awk '{ print $4 }' | sort -n | uniq -c
