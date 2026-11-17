#!/bin/sh
# intel_replacements.sh - build replacement libraries (i386) into the Intel root for what the 10.5.8 Combo Update lacks:
# vImage.framework, libBLAS.dylib, libLAPACK.dylib (Accelerate/vecLib load them; without them every program
# that uses ApplicationServices fails). Install names and versions as in Apple's (otool -L Accelerate/vecLib).
# MLTEFile.bundle: MLTE's RTF plug-in (TXNSetDataFromCFURLRef with RTF, licence dialogs of Aspyr's games).
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
M=/System/Library/CoreServices/MLTEFile.bundle/Contents
mkdir -p "$W$M/MacOS"
gcc-4.2 -arch i386 -O2 -bundle -isysroot $S -Wall -Wno-deprecated-declarations -o "$W$M/MacOS/MLTEFile" \
    "$Q/mltefile.m" -framework AppKit
cat > "$W$M/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key>
	<string>MLTEFile</string>
	<key>CFBundleIdentifier</key>
	<string>com.apple.MLTEFile</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundlePackageType</key>
	<string>BNDL</string>
	<key>CFBundleVersion</key>
	<string>3.0.0</string>
</dict>
</plist>
EOF
echo "MLTEFile: $(nm -g "$W$M/MacOS/MLTEFile" | grep -c ' T _MLTEFile_') functions"
for f in "$W$A/Frameworks/vImage.framework/Versions/A/vImage" "$W$V/libBLAS.dylib" "$W$V/libLAPACK.dylib"; do
    echo "$(basename "$f"): $(nm -g "$f" | grep -c ' T ') functions"
done
