#!/bin/sh
# intel_prune.sh - remove from intel/root what the system itself already has with an i386 part (libSystem, libobjc,
# dyld ...): then the system version applies, which matches the running kernel exactly. Files are removed only in
# intel/root.
R=$(cd "$(dirname "$0")/.." && pwd)
W=$R/intel/root
cd "$W" || exit 1
n=0; b=0
find . -type f | sed 's|^\./|/|' > /tmp/ib_files.txt
while read -r p; do
  if [ -f "$p" ] && lipo -info "$p" 2>/dev/null | grep -q i386; then
    rm -f "$W$p"; n=$((n + 1))
  else
    b=$((b + 1))
  fi
done < /tmp/ib_files.txt
echo "$n removed (system has i386), $b kept"
find "$W" -type d -empty -delete 2>/dev/null
du -sh "$W"
