#!/bin/sh
# intel_quicktime.sh DMG - the i386 part of Apple's QuickTime 7.6.4 for Leopard (QuickTime764_Leopard.dmg, universal) into
# the Intel root, without installing it: QuickTime, QTKit and CoreVideo (replacing their stub libraries), the CoreMedia
# private frameworks and the codec components. Download (Apple):
#   https://secure-appldnld.apple.com/QuickTime/061-6738.20090909.tq764/QuickTime764_Leopard.dmg  (56.5 MB, SHA-1
#   f35a8f6f09ee884405ae10b1eca4b7c059f274f0; page https://support.apple.com/en-ca/106568)
# Steps: mount read-only, copy the bundle package, extract its payload into intel/raw_quicktime, thin i386 copies into
# the root (tools/intel_root.sh), give the new libraries fixed addresses above the existing ones (the others keep theirs:
# intel_patch.sh finds its sites by address), prebind, check the patches. AppleVAH264HW.component (hardware H.264
# decoding through IOKit with the graphics driver, made for Intel graphics) stays out of the root. A backup of the root
# goes to intel/backup first.
R=$(cd "$(dirname "$0")/.." && pwd)
D=$1
W=$R/intel/root
[ -f "$D" ] || { echo "usage: intel_quicktime.sh QuickTime764_Leopard.dmg"; exit 1; }
openssl sha1 "$D"
mkdir -p "$R/intel/backup" "$R/intel/packages/quicktime" "$R/intel/raw_quicktime"
[ -f "$R/intel/backup/root-before-quicktime.tar" ] || tar cf "$R/intel/backup/root-before-quicktime.tar" -C "$W/" . || exit 1
M=/tmp/qt_mount.$$
mkdir -p $M
hdiutil attach -readonly -nobrowse -noverify -mountpoint $M "$D" < /dev/null > /dev/null || { rmdir $M; exit 1; }
rm -rf "$R/intel/packages/quicktime/QuickTime764_Leopard.pkg"
cp -R $M/QuickTime764_Leopard.pkg "$R/intel/packages/quicktime/"
hdiutil detach $M > /dev/null; rmdir $M
(cd "$R/intel/raw_quicktime" && gzip -dc "$R/intel/packages/quicktime/QuickTime764_Leopard.pkg/Contents/Archive.pax.gz" | pax -r)
sh "$R/tools/intel_root.sh" "$R/intel/raw_quicktime" | tail -1
# directory links (Versions/Current, Resources) that cp -P without -R could not take
(cd "$R/intel/raw_quicktime" && find System/Library -type l) | while read -r l; do
    [ -d "$W/$(dirname "$l")" ] || continue
    [ -e "$W/$l" ] || [ -h "$W/$l" ] || ln -s "$(readlink "$R/intel/raw_quicktime/$l")" "$W/$l"
done
mkdir -p "$R/intel/backup/quicktime_disabled"
rm -rf "$R/intel/backup/quicktime_disabled/AppleVAH264HW.component"
[ -d "$W/System/Library/QuickTime/AppleVAH264HW.component" ] &&
    mv "$W/System/Library/QuickTime/AppleVAH264HW.component" "$R/intel/backup/quicktime_disabled/"
# fixed addresses for the new libraries above the highest one in the root
L=/tmp/iq_new.txt
: > $L
for f in Frameworks/QuickTime.framework/Versions/A/QuickTime Frameworks/QTKit.framework/Versions/A/QTKit \
         Frameworks/CoreVideo.framework/Versions/A/CoreVideo PrivateFrameworks/CoreAUC.framework/Versions/A/CoreAUC \
         PrivateFrameworks/CoreMediaPrivate.framework/Versions/A/CoreMediaPrivate \
         PrivateFrameworks/CoreMediaAuthoringPrivate.framework/Versions/A/CoreMediaAuthoringPrivate \
         PrivateFrameworks/CoreMediaIOServicesPrivate.framework/Versions/A/CoreMediaIOServicesPrivate \
         PrivateFrameworks/FWAVCPrivate.framework/Versions/A/FWAVCPrivate; do
    [ -f "$W/System/Library/$f" ] && echo "$W/System/Library/$f" >> $L
done
top=$(find "$W/" -type f -name '*.dylib' -o -type f -path '*.framework/Versions/*' | grep -v -f $L | while read -r f; do
          otool -l "$f" 2>/dev/null | perl -ne '$a = hex $1 if /vmaddr (0x[0-9a-f]+)/; if (/vmsize (0x[0-9a-f]+)/) { $e = $a + hex $1; $h = $e if $e > $h } END { printf "%d\n", $h }'
      done | sort -n | tail -1)
A=$(perl -e 'printf "%x", (($ARGV[0] + 0xfffff) & ~0xfffff)' "$top")
echo "new libraries from 0x$A"
chmod u+w $(cat $L)
rebase -low_address "$A" -arch i386 $(cat $L) 2>&1 | tail -2
sh "$R/tools/intel_prebind.sh" | tail -1
sh "$R/tools/intel_patch.sh" | tail -4
