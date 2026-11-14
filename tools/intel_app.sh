#!/bin/sh
# intel_app.sh NAME [SUBFOLDER] - assemble an application bundle with an Intel part for rosetta: the Combo Update
# contains only the changed files (program, plug-ins), the system has the complete bundle of the same version.
# Result: $R/intel/apps/NAME.app = copy of /Applications[/SUBFOLDER], with the files from
# the update on top. /Applications stays unchanged.
R=$(cd "$(dirname "$0")/.." && pwd)
N=$1; U=${2:+$2/}
Q=/Applications/$U$N.app
P=$R/intel/raw/Applications/$U$N.app
Z=$R/intel/apps/$N.app
[ -d "$Q" ] || { echo "no $Q"; exit 1; }
[ -d "$P" ] || { echo "no $P (not in the update)"; exit 1; }
mkdir -p "$(dirname "$Z")"
rm -rf "$Z"
cp -Rp "$Q" "$Z"
( cd "$P" && find . -type f ) | while read f; do mkdir -p "$(dirname "$Z/$f")"; cp -p "$P/$f" "$Z/$f"; done
b=$(defaults read "$Z/Contents/Info" CFBundleExecutable)
echo "$Z: $(find "$Z" -type f | wc -l | tr -d ' ') files, executable $(lipo -info "$Z/Contents/MacOS/$b" | sed 's/.*: //')"
