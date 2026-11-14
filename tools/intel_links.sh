#!/bin/sh
# intel_links.sh - carry symbolic links over from intel/raw to intel/root (Versions/Current, Framework/Name,
# lib*.dylib) where the directory exists in the Intel root (on Leopard, cp -P without -R follows the link)
R=$(cd "$(dirname "$0")/.." && pwd)
Q=$R/intel/raw
W=$R/intel/root
cd "$Q" || exit 1
n=0
find System/Library Library usr/lib -type l 2>/dev/null > /tmp/iv_links.txt
while read -r l; do
  [ -d "$W/$(dirname "$l")" ] || continue
  [ -e "$W/$l" ] || [ -h "$W/$l" ] && continue
  ln -s "$(readlink "$l")" "$W/$l" && n=$((n + 1))
done < /tmp/iv_links.txt
echo "$n links created"
