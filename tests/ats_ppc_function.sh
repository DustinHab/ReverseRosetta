#!/bin/sh
# ats_ppc_function.sh FUNCTION [LINES] [server] - function from the system's PowerPC ATS (client framework or ATSServer)
D=/System/Library/Frameworks/ApplicationServices.framework/Versions/A/Frameworks/ATS.framework/Versions/A
A=$D/ATS
[ "$3" = server ] && A=$D/Support/ATSServer
C=/tmp/ats_ppc_$3.txt
[ -s $C ] || otool -tV -arch ppc $A > $C
awk -v f="$1:" -v n="${2:-80}" '$0 == f { p = 1 } p { print; if (++k > n) exit }' $C
