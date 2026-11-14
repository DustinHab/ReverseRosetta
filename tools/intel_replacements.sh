#!/bin/sh
# intel_replacements.sh - build replacement libraries (i386) into the Intel root for what the 10.5.8 Combo Update lacks:
# vImage.framework, libBLAS.dylib, libLAPACK.dylib (Accelerate/vecLib load them; without them every program
# that uses ApplicationServices fails). Install names and versions as in Apple's (otool -L Accelerate/vecLib).
R=$(cd "$(dirname "$0")/.." && pwd)
set -e
Q=$(cd "$(dirname "$0")" && pwd)/replacements
W=${ROSETTA_INTEL:-$R/intel/root}
S=/Developer/SDKs/MacOSX10.5.sdk
A=/System/Library/Frameworks/Accelerate.framework/Versions/A
V=$A/Frameworks/vecLib.framework/Versions/A
CF="-arch i386 -O2 -dynamiclib -isysroot $S -Wall"
mkdir -p "$W$A/Frameworks/vImage.framework/Versions/A" "$W$V"
gcc-4.2 $CF -install_name $A/Frameworks/vImage.framework/Versions/A/vImage -compatibility_version 1.0.0 \
    -current_version 139.0.0 -o "$W$A/Frameworks/vImage.framework/Versions/A/vImage" "$Q/vimage.c"
gcc-4.2 $CF -install_name $V/libBLAS.dylib -compatibility_version 1.0.0 -current_version 218.0.0 \
    -o "$W$V/libBLAS.dylib" "$Q/blas.c"
gcc-4.2 $CF -install_name $V/libLAPACK.dylib -compatibility_version 1.0.0 -current_version 218.0.0 \
    -o "$W$V/libLAPACK.dylib" "$Q/lapack.c"
( cd "$W$A/Frameworks/vImage.framework" && ln -sfh A Versions/Current && ln -sfh Versions/Current/vImage vImage )
for f in "$W$A/Frameworks/vImage.framework/Versions/A/vImage" "$W$V/libBLAS.dylib" "$W$V/libLAPACK.dylib"; do
    echo "$(basename "$f"): $(nm -g "$f" | grep -c ' T ') functions"
done
