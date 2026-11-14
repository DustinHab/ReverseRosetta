#!/bin/sh
# stubs_rebuild.sh - rebuild all stub libraries of the Intel root with the symbols of all clients: the starting points
# are the i386 files of every program in intel/apps/ (main executable, embedded frameworks, plug-ins) and
# Cocoa (test programs). Needed when a new program needs more symbols from an existing stub library.
# deps.sh recognizes stub libraries by the marker "rosetta-stub" (STUBS_REBUILD=1).
R=$(cd "$(dirname "$0")/.." && pwd)
D=$(cd "$(dirname "$0")" && pwd)
for a in $R/intel/apps/*.app; do
    find "$a/Contents" -type f -perm -u+x 2>/dev/null | while read f; do
        lipo -info "$f" 2>/dev/null | grep -q i386 && echo "$f"
    done
done > /tmp/pe_paths.txt
echo /System/Library/Frameworks/Cocoa.framework/Versions/A/Cocoa >> /tmp/pe_paths.txt
echo "$(wc -l < /tmp/pe_paths.txt | tr -d ' ') starting points"
IFS='
'
STUBS_REBUILD=1 sh "$D/intel_stubs.sh" $(cat /tmp/pe_paths.txt)
