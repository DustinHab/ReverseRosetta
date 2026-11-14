#!/bin/sh
# who_loads.sh PATTERN - which i386 images of the Intel root load a library whose path contains PATTERN
R=$(cd "$(dirname "$0")/.." && pwd)
W=$R/intel/root
find $W/System/Library/Frameworks $W/System/Library/PrivateFrameworks $W/usr/lib -type f 2>/dev/null | while read f; do
    otool -L "$f" 2>/dev/null | tail -n +2 | grep -q "$1" && echo "${f#$W}"
done
