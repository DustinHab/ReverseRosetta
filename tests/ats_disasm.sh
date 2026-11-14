#!/bin/sh
# ats_disasm.sh FUNCTION|ADDR [LINES] - disassemble Intel's ATS.framework (Intel root)
R=$(cd "$(dirname "$0")/.." && pwd)
A=$R/intel/root/System/Library/Frameworks/ApplicationServices.framework/Versions/A/Frameworks/ATS.framework/Versions/A/ATS
otool -tV -arch i386 $A | awk -v f="$1" -v n="${2:-60}" '($0 == f ":" || index($1, f) == 1) { p = 1 } p { print; if (++k > n) exit }'
