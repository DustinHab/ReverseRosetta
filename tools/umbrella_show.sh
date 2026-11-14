#!/bin/sh
# umbrella_show.sh NAME - show an umbrella framework in the SDK (i386 stub) and in the system: ID, versions,
# subframeworks, own exported symbols
N=${1:-CoreServices}
S=/Developer/SDKs/MacOSX10.5.sdk/System/Library/Frameworks/$N.framework/Versions/A/$N
echo "== system (PPC):"; otool -L /System/Library/Frameworks/$N.framework/Versions/A/$N | head -3
echo "== SDK stub i386, load commands:"
otool -arch i386 -l "$S" | grep -A3 -E 'LC_SUB_UMBRELLA|LC_REEXPORT|LC_ID_DYLIB|LC_LOAD_DYLIB|LC_SUB_CLIENT' | grep -E 'cmd |name|umbrella|current|compat' | head -60
echo "== own symbols (defined, external):"
nm -arch i386 -g "$S" | grep -v ' U ' | head -20
echo "count: $(nm -arch i386 -g "$S" | grep -vc ' U ')"
