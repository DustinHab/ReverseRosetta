#!/bin/sh
# stubs_rebuild.sh - rebuild all stub libraries of the Intel root with the symbols of all clients: the starting points
# are the i386 files of every program in intel/apps/ (main executable, embedded frameworks, plug-ins), Cocoa (test
# programs) and the universal command line programs of the system (/bin, /usr/bin, /usr/sbin). Needed when a new
# program needs more symbols from an existing stub library. deps.sh recognizes stub libraries by their marker
# (STUBS_REBUILD=1). The previous stub libraries are saved first (intel/backup/stubs-DATE.tar).
R=$(cd "$(dirname "$0")/.." && pwd)
D=$(cd "$(dirname "$0")" && pwd)
for a in $R/intel/apps/*.app; do
    find "$a/Contents" -type f -perm -u+x 2>/dev/null | while read f; do
        lipo -info "$f" 2>/dev/null | grep -q i386 && echo "$f"
    done
done > /tmp/pe_paths.txt
echo /System/Library/Frameworks/Cocoa.framework/Versions/A/Cocoa >> /tmp/pe_paths.txt
for f in /bin/* /usr/bin/* /usr/sbin/*; do
    [ -f "$f" ] && [ -x "$f" ] && lipo -info "$f" 2>/dev/null | grep -q i386 && echo "$f"
done >> /tmp/pe_paths.txt
echo "$(wc -l < /tmp/pe_paths.txt | tr -d ' ') starting points"
W=${ROSETTA_INTEL:-$R/intel/root}
( cd "$W" && find . -type f -perm -u+x -size -400k 2>/dev/null | while read f; do
      grep -q -e "rosetta-stub" -e "rosetta-Platzhalter" "$f" && echo "$f"
  done > /tmp/pe_stubs.txt
  [ -s /tmp/pe_stubs.txt ] && mkdir -p "$R/intel/backup" && tar cf "$R/intel/backup/stubs-$(date +%Y%m%d-%H%M%S).tar" -T /tmp/pe_stubs.txt &&
      echo "$(wc -l < /tmp/pe_stubs.txt | tr -d ' ') stub libraries saved" )
IFS='
'
STUBS_REBUILD=1 sh "$D/intel_stubs.sh" $(cat /tmp/pe_paths.txt)
