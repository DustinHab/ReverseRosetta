#!/bin/sh
# sdk_stubs_all.sh [-n] - stub libraries with every symbol of the 10.5 SDK for each public library that a translated
# program would not find as i386: neither the system file has an i386 part nor the Intel root a real copy (QuickTime,
# QTKit, CoreAudioKit, OpenAL, ForceFeedback, InputMethodKit, JavaVM ...), and the existing stub libraries completed
# with the SDK's symbols. Without it a program stops in dyld at the first symbol that no earlier program needed
# ("Symbol not found: _QTMovieCurrentTimeAttribute": Keynote, Pages, Numbers). -n: only list.
R=$(cd "$(dirname "$0")/.." && pwd)
W=${ROSETTA_INTEL:-$R/intel/root}
SDK=/Developer/SDKs/MacOSX10.5.sdk
list=0
[ "$1" = -n ] && list=1
cd $SDK || exit 1
find System/Library/Frameworks usr/lib -type f 2>/dev/null | while read -r f; do
    case "$f" in
        *.h|*.plist|*/Headers/*|*/Resources/*|*.strings|*.a|*.o) continue ;;
        usr/lib/*-apple-darwin*/*) continue ;;  # compiler support libraries of the SDK, not looked up at run time
    esac
    file "$f" | grep -q 'Mach-O' || continue
    lipo -info "$f" 2>/dev/null | grep -q i386 || continue
    m=/$f
    if [ -f "$W$m" ]; then
        grep -q -e rosetta-stub -e rosetta-Platzhalter "$W$m" && echo "stub	$m"
        continue
    fi
    lipo -info "$m" 2>/dev/null | grep -q i386 && continue
    echo "missing	$m"
done | sort > /tmp/sdk_stubs_all.txt
if [ $list = 1 ]; then cat /tmp/sdk_stubs_all.txt; exit 0; fi
cut -f2 /tmp/sdk_stubs_all.txt | while read -r m; do sh "$R/tools/sdk_stub.sh" "$m"; done
