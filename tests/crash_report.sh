#!/bin/sh
# crash_report.sh - last rosetta crash report: exception, crashed thread, registers
f=$(ls -t ~/Library/Logs/CrashReporter/rosetta_*.crash 2>/dev/null | head -1)
echo "$f"
grep -E "^Exception|^Crashed Thread|^Thread [0-9]+ Crashed" "$f"
awk '/Crashed:/ { p = 1 } p { print; if (++k > 14) exit }' "$f"
awk '/Thread State/ { p = 1 } p { print; if (++k > 14) exit }' "$f"
