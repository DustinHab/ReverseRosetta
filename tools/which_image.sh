#!/bin/sh
# which_image.sh ADDRESS... - the i386 library of the Intel root whose __TEXT contains each address (from a crash
# report of rosetta: eip, call chain). The libraries sit at fixed addresses (intel_rebase.sh); the first call collects
# the address map into intel/image_map.txt (a few minutes), REMAP=1 collects it again.
R=$(cd "$(dirname "$0")/.." && pwd)
W=$R/intel/root
MAP=$R/intel/image_map.txt
if [ ! -s "$MAP" ] || [ -n "$REMAP" ]; then
  echo "collecting the address map of $W ..." >&2
  (cd "$W" && find . -type f \( -name '*.dylib' -o -perm -100 \) 2>/dev/null) | while IFS= read -r f; do
    otool -arch i386 -l "$W/${f#./}" 2>/dev/null | awk -v f="/${f#./}" '
      /segname __TEXT$/ { t = 1; next }
      t && $1 == "vmaddr" { a = $2 }
      t && $1 == "vmsize" { printf "%s %s\t%s\n", a, $2, f; exit }'
  done | sort > "$MAP"
  echo "$(wc -l < "$MAP") images" >&2
fi
for x in "$@"; do
  x=$(echo "$x" | sed 's/^0x//' | tr 'A-F' 'a-f')
  awk -v x="$x" -F'\t' '
    function hex(s,   i, c, v) { v = 0; s = tolower(s); sub(/^0x/, "", s)
      for (i = 1; i <= length(s); i++) { c = index("0123456789abcdef", substr(s, i, 1)) - 1; v = v * 16 + c }
      return v }
    BEGIN { a = hex(x) }
    { split($1, r, " "); b = hex(r[1]); e = b + hex(r[2])
      if (b > 0 && a >= b && a < e) { printf "%s  %s +0x%x\n", x, $2, a - b; found = 1; exit } }
    END { if (!found) print x "  (no image of the Intel root)" }' "$MAP"
done
