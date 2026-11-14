#!/bin/sh
# who_needs.sh NAME - which i386 images of the Intel root load NAME (library/framework), and what they take from it
R=$(cd "$(dirname "$0")/.." && pwd)
W=$R/intel/root
CG=$W/System/Library/Frameworks/ApplicationServices.framework/Versions/A/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics
otool -L $CG | grep -i -E "accelerate|veclib|vimage"
nm -m $CG | grep undefined | awk '{print $NF}' | sort | uniq -c | sort -rn | head -30
