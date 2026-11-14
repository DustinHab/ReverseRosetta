#!/bin/sh
# app_stubs.sh NAME - missing i386 libraries of an assembled program (including plug-ins
# and embedded frameworks) as stub libraries in the Intel root (intel_stubs.sh)
R=$(cd "$(dirname "$0")/.." && pwd)
Z=$R/intel/apps/$1.app
L=$(find "$Z/Contents" -type f -perm -u+x 2>/dev/null | while read f; do lipo -info "$f" 2>/dev/null | grep -q i386 && echo "$f"; done)
echo "$(echo "$L" | wc -l | tr -d ' ') executable images with i386"
sh "$(dirname "$0")/intel_stubs.sh" $L
