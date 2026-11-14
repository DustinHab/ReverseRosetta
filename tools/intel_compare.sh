#!/bin/sh
# intel_compare.sh - i386 parts of important system libraries: system against Combo Update 10.5.8 (same/different)
R=$(cd "$(dirname "$0")/.." && pwd)
Q=$R/intel/raw
for p in /usr/lib/libobjc.A.dylib /usr/lib/libSystem.B.dylib /usr/lib/libicucore.A.dylib /usr/lib/libauto.dylib \
         /usr/lib/system/libmathCommon.A.dylib /usr/lib/libgcc_s.1.dylib /usr/lib/libstdc++.6.dylib /usr/lib/dyld \
         /usr/lib/libxml2.2.dylib /usr/lib/libz.1.dylib; do
  [ -f "$Q$p" ] || { echo "$p: not in the update"; continue; }
  lipo -thin i386 "$p" -output /tmp/iv_sys 2>/dev/null || { echo "$p: system without i386"; continue; }
  lipo -thin i386 "$Q$p" -output /tmp/iv_upd 2>/dev/null
  if cmp -s /tmp/iv_sys /tmp/iv_upd; then echo "$p: same"
  else echo "$p: DIFFERENT (system $(otool -L /tmp/iv_sys | sed -n 2p | sed 's/.*current version //'), update $(otool -L /tmp/iv_upd | sed -n 2p | sed 's/.*current version //'))"; fi
done
