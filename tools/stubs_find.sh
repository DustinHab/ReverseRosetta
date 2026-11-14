#!/bin/sh
# stubs_find.sh - small i386 libraries of the Intel root that link only CoreFoundation and libSystem
# (this is what stub libraries look like); marked by whether they contain the marker "rosetta-stub"
R=$(cd "$(dirname "$0")/.." && pwd)
W=${ROSETTA_INTEL:-$R/intel/root}
find $W -type f -size -64k 2>/dev/null | while read f; do
    lipo -info "$f" 2>/dev/null | grep -q i386 || continue
    otool -arch i386 -L "$f" 2>/dev/null | tail -n +3 | awk '{ print $1 }' | grep -v -E "CoreFoundation|libSystem|libgcc_s" | grep -q . && continue
    otool -arch i386 -D "$f" 2>/dev/null | tail -n +2 | grep -q . || continue     # libraries only (with an ID)
    grep -q "rosetta-stub" "$f" && k=marker || k=none
    echo "$k $(ls -l "$f" | awk '{ print $5 }') ${f#$W}"
done
