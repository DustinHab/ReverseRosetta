#!/bin/sh
# install_app.sh SOURCE [NAME] - set up an Intel program for rosetta in one step. SOURCE is a disk image (.dmg), a
# zip archive, an application bundle (.app) or the name of an application of the Combo Update (intel/raw/Applications,
# e.g. Preview or "Activity Monitor"). The program is copied to intel/apps/NAME.app; missing i386 libraries get stub
# libraries (stubs_rebuild.sh); then a Finder launcher is made (finder_app.sh: ~/Applications/Intel/NAME.app).
# Nothing outside the repository and ~/Applications/Intel is changed.
R=$(cd "$(dirname "$0")/.." && pwd)
T=$R/tools
S=$1; N=$2
Z=$R/intel/apps
[ -n "$S" ] || { echo "usage: install_app.sh SOURCE [NAME]"; exit 1; }
mkdir -p "$Z"
take() {    # copy application bundle $1 to intel/apps (name from $N or the bundle)
    [ -n "$N" ] || N=$(basename "$1" .app)
    rm -rf "$Z/$N.app"
    ditto "$1" "$Z/$N.app" || exit 1
}
case "$S" in
*.dmg)
    M=/tmp/install_app.$$
    mkdir -p $M
    hdiutil attach -readonly -nobrowse -noverify -mountpoint $M "$S" > /dev/null || { rmdir $M; exit 1; }
    A=$(find $M -maxdepth 2 -name '*.app' -type d | head -1)
    if [ -n "$A" ]; then take "$A"; fi
    hdiutil detach $M > /dev/null
    rmdir $M
    [ -n "$A" ] || { echo "no application in $S"; exit 1; } ;;
*.zip)
    X=/tmp/install_app.$$
    mkdir -p $X
    ditto -x -k "$S" $X || { rm -rf $X; exit 1; }
    A=$(find $X -maxdepth 3 -name '*.app' -type d | head -1)
    if [ -n "$A" ]; then take "$A"; fi
    rm -rf $X
    [ -n "$A" ] || { echo "no application in $S"; exit 1; } ;;
*.app|*.app/)
    [ -d "$S" ] || { echo "no $S"; exit 1; }
    take "${S%/}" ;;
*)                                          # an application of the Combo Update, at the top or in a subfolder
    A=$(find "$R/intel/raw/Applications" -maxdepth 2 -name "$S.app" -type d | head -1)
    [ -n "$A" ] || { echo "$S is neither a file nor an application of the update (intel/raw/Applications)"; exit 1; }
    sub=$(dirname "$A" | sed "s#^$R/intel/raw/Applications##; s#^/##")
    sh "$T/intel_app.sh" "$S" "$sub" || exit 1
    N=$S ;;
esac
P=$Z/$N.app
b=$(defaults read "$P/Contents/Info" CFBundleExecutable) || exit 1
arch=$(lipo -info "$P/Contents/MacOS/$b" 2>/dev/null | sed 's/.*: //')
case "$arch" in
*i386*) ;;
*) echo "$N: no i386 part ($arch)"; exit 1 ;;
esac
case "$arch" in *ppc*) echo "note: $N has a PowerPC part as well, it also runs natively" ;; esac
echo "== $N $(defaults read "$P/Contents/Info" CFBundleShortVersionString 2>/dev/null), $arch"
sh "$T/app_deps.sh" "$N" | grep -v '^   from' | head -30
# stub libraries: new ones for what is missing, and the existing ones this program uses rebuilt with its symbols
# added (they keep the old ones); only this program's dependencies are looked at
echo "== stub libraries"
find "$P/Contents" -type f -perm -u+x 2>/dev/null | while read f; do
    lipo -info "$f" 2>/dev/null | grep -q i386 && echo "$f"
done > /tmp/install_app_paths.txt
IFS='
'
STUBS_REBUILD=1 sh "$T/intel_stubs.sh" $(cat /tmp/install_app_paths.txt) 2>&1 | tail -8
unset IFS
# what each symbol is (function or data) from the SDK or the PowerPC library, Objective-C classes with their superclass
sh "$T/stubs_rebuild_all.sh" > /dev/null 2>&1
echo "== launcher"
sh "$T/finder_app.sh" "$N"
