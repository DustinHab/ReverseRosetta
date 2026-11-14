#!/bin/sh
# objc_symbols.sh - local symbols of the i386 libobjc (for native stubs), plus the size of the built-in selector table
R=$(cd "$(dirname "$0")/.." && pwd)
L=$R/intel/root/usr/lib/libobjc.A.dylib
nm -arch i386 $L | grep -E "objc_search_builtins|classHash|classIsEqual|_lookup$|_phash|NXHashGet|builtin_selectors|_objc_strhash|lookupNamedMethodInMethodList|class_getMethodNoSuper" | head -20
nm -arch i386 $L | wc -l
otool -arch i386 -tV $L > /tmp/objc_dis.txt 2>/dev/null
awk '/^__objc_search_builtins:/,/^_[a-zA-Z_].*:$/' /tmp/objc_dis.txt | head -60
