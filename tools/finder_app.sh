#!/bin/sh
# finder_app.sh NAME [FOLDER] - a launcher for the Finder, the Dock and "open": FOLDER/NAME.app (default
# ~/Applications/Intel) starts intel/apps/NAME.app through rosetta on a double click. It is a small bundle with the
# program's Info.plist and icon (Resources is a link to the original) whose executable is a shell script; the
# program itself still runs from intel/apps (its main bundle). Run it again after moving the repository.
R=$(cd "$(dirname "$0")/.." && pwd)
N=$1
[ -n "$N" ] || { echo "usage: finder_app.sh NAME [FOLDER]"; exit 1; }
A=$R/intel/apps/$N.app
[ -d "$A" ] || { echo "no $A"; exit 1; }
b=$(defaults read "$A/Contents/Info" CFBundleExecutable) || exit 1
F=${2:-$HOME/Applications/Intel}
W=$F/$N.app
mkdir -p "$F"
rm -rf "$W"
mkdir -p "$W/Contents/MacOS"
cp "$A/Contents/Info.plist" "$W/Contents/Info.plist"
# the executable is a script; keys that ask for a particular architecture would make Launch Services refuse it
defaults write "$W/Contents/Info" CFBundleExecutable rosetta-launch
for k in LSArchitecturePriority LSRequiresNativeExecution LSMinimumSystemVersionByArchitecture LSRequiresCarbon; do
    defaults delete "$W/Contents/Info" $k 2>/dev/null
done
plutil -convert xml1 "$W/Contents/Info.plist"
ln -s "$A/Contents/Resources" "$W/Contents/Resources"
[ -f "$A/Contents/PkgInfo" ] && cp "$A/Contents/PkgInfo" "$W/Contents/PkgInfo"
# exec: the program keeps the process that Launch Services started (and its -psn argument)
cat > "$W/Contents/MacOS/rosetta-launch" <<EOF
#!/bin/sh
exec "$R/build/rosetta" "$A/Contents/MacOS/$b" "\$@"
EOF
chmod 755 "$W/Contents/MacOS/rosetta-launch"
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$W"
echo "$W"
