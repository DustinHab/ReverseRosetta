#!/bin/sh
# intel_root.sh - from the unpacked Combo Update (intel/raw), put every Mach-O file with an i386 part as a thin
# i386 version under the same path into intel/root (rosetta redirects the guest's accesses there when the file
# exists there). Symbolic links in frameworks are taken along. Nothing is installed.
R=$(cd "$(dirname "$0")/.." && pwd)
Q=$R/intel/raw
W=$R/intel/root
cd "$Q" || exit 1
mkdir -p "$W"
n=0; d=0
find System/Library Library usr/lib -type f 2>/dev/null > /tmp/iw_files.txt
while read -r f; do
  m=$(head -c 4 "$f" 2>/dev/null | od -An -tx1 | tr -d ' \n')
  case "$m" in
    cafebabe) lipo -info "$f" 2>/dev/null | grep -q i386 || continue
              mkdir -p "$W/$(dirname "$f")"
              lipo -thin i386 "$f" -output "$W/$f" && n=$((n + 1)) ;;
    cefaedfe) mkdir -p "$W/$(dirname "$f")"          # already thin i386
              cp -p "$f" "$W/$f" && d=$((d + 1)) ;;
  esac
done < /tmp/iw_files.txt
# links (Versions/Current, Framework/Name, lib*.dylib -> lib*.x.dylib) where the directory exists in the root
v=0
find System/Library Library usr/lib -type l 2>/dev/null | while read -r l; do
  [ -d "$W/$(dirname "$l")" ] || continue
  [ -e "$W/$l" ] || [ -h "$W/$l" ] || { cp -P "$l" "$W/$l" && v=$((v + 1)); }
done
echo "$n from fat files, $d already thin"
echo "links: $(find "$W" -type l | wc -l)"
du -sh "$W"
