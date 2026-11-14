#!/bin/sh
# thirdparty_unpack.sh DMG NAME - copy a third-party program (Intel only) from a .dmg to
# intel/apps/NAME.app (image mounted read-only and without Finder, unmounted again afterwards);
# shows architecture and dependencies
R=$(cd "$(dirname "$0")/.." && pwd)
D=$1; N=$2
Z=$R/intel/apps
M=/tmp/thirdparty_dmg.$$
mkdir -p $Z $M
hdiutil attach -readonly -nobrowse -noverify -mountpoint $M "$D" > /dev/null || exit 1
A=$(ls -d $M/*.app | head -1)
rm -rf "$Z/$N.app"
ditto "$A" "$Z/$N.app"
hdiutil detach $M > /dev/null
rmdir $M
P="$Z/$N.app"
b=$(defaults read "$P/Contents/Info" CFBundleExecutable)
echo "== $P"
defaults read "$P/Contents/Info" CFBundleShortVersionString
defaults read "$P/Contents/Info" LSMinimumSystemVersion 2>/dev/null
file "$P/Contents/MacOS/$b"
lipo -info "$P/Contents/MacOS/$b"
echo "== dependencies"
otool -arch i386 -L "$P/Contents/MacOS/$b" | tail -n +2
echo "== bundled frameworks"
ls "$P/Contents/Frameworks" 2>/dev/null
for f in "$P"/Contents/Frameworks/*.framework; do
    [ -d "$f" ] || continue
    n=$(basename "$f" .framework)
    lipo -info "$f/$n"
done
