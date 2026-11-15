#!/bin/sh
# intel_umbrellas.sh [NAME...] - rebuild missing umbrella shells (CoreServices, ApplicationServices, Carbon, Cocoa) as
# i386: an empty library with the ID and versions of the SDK stub that re-exports every subframework. Subframeworks
# that exist neither in intel/root nor in the system as i386 (DictionaryServices, Help, ImageKit ...) first get a stub
# library with all their symbols from the SDK (sdk_stub.sh): otherwise programs that take one of these symbols from
# the umbrella do not load ("Symbol not found: _AHRegisterHelpBook ... Expected in: Carbon"). UMBRELLA_NO_STUBS=1:
# leave them out as before.
R=$(cd "$(dirname "$0")/.." && pwd)
SDK=/Developer/SDKs/MacOSX10.5.sdk
W=$R/intel/root
echo "" > /tmp/umbrella_empty.c
has_i386() {   # path in the guest: true if it exists as i386 (root or system)
  [ -f "$W$1" ] && return 0
  lipo -info "$1" 2>/dev/null | grep -q i386
}
for N in ${*:-CoreServices ApplicationServices Carbon Cocoa}; do
  S=$SDK/System/Library/Frameworks/$N.framework/Versions/A/$N
  [ -f "$S" ] || { echo "$N: no SDK stub"; continue; }
  id=$(otool -arch i386 -D "$S" | tail -1)
  ver=$(otool -arch i386 -L "$S" | sed -n 2p)
  compat=$(echo "$ver" | sed 's/.*compatibility version \([0-9.]*\).*/\1/')
  cur=$(echo "$ver" | sed 's/.*current version \([0-9.]*\).*/\1/')
  args=""; with=""; without=""
  for r in $(otool -arch i386 -l "$S" | awk '/LC_REEXPORT_DYLIB/ { z = 1 } z && /name/ { print $2; z = 0 }'); do
    [ -z "$UMBRELLA_NO_STUBS" ] && ! has_i386 "$r" && sh "$(dirname "$0")/sdk_stub.sh" "$r" | sed 's/^/   stub: /'
    if has_i386 "$r"; then args="$args -Wl,-reexport_library,$SDK$r"; with="$with $(basename $r)"
    else without="$without $(basename $r)"; fi
  done
  mkdir -p "$W$(dirname "$id")"
  if gcc-4.2 -arch i386 -dynamiclib -isysroot $SDK /tmp/umbrella_empty.c -o "$W$id" -install_name "$id" \
       -compatibility_version "$compat" -current_version "$cur" $args; then
    echo "$N $cur: re-exported:$with"
    [ -n "$without" ] && echo "   missing as i386:$without"
  else
    echo "$N: build failed"
  fi
done
