#!/bin/sh
# intel_unpack.sh DMG - unpack Apple's Mac OS X 10.5.8 Combo Update (MacOSXUpdCombo10.5.8.dmg) without installing it:
# mount the image read-only, show the package signature, expand the flat package into intel/packages/combo and extract
# its payload into intel/raw. The running system is not changed. Needs about 3 GB of free disk space.
# Afterwards: tools/intel_root.sh (see README.md for the full order).
R=$(cd "$(dirname "$0")/.." && pwd)
D=$1
[ -f "$D" ] || { echo "usage: intel_unpack.sh MacOSXUpdCombo10.5.8.dmg"; exit 1; }
M=/tmp/combo_mount.$$
mkdir -p $M "$R/intel/packages" "$R/intel/raw"
hdiutil attach -readonly -nobrowse -noverify -mountpoint $M "$D" > /dev/null || { rmdir $M; exit 1; }
P=$(ls -d $M/*.pkg | head -1)
sh "$R/tools/pkg_signature.sh" "$P"
rm -rf "$R/intel/packages/combo"
pkgutil --expand "$P" "$R/intel/packages/combo"
hdiutil detach $M > /dev/null
rmdir $M
L=$(ls "$R"/intel/packages/combo/*.pkg/Payload | head -1)
[ -f "$L" ] || { echo "no payload found in intel/packages/combo"; exit 1; }
cd "$R/intel/raw" && gzip -dc "$L" | cpio -id 2>&1 | tail -1
du -sh "$R/intel/raw"
