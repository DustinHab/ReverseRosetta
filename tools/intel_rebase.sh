#!/bin/sh
# intel_rebase.sh [ADDRESS] - move the i386 libraries of the Intel root to fixed, free addresses (Apple's rebase).
# Leopard's i386 libraries are linked at address 0 (on the Intel Mac they come from the shared cache); when they are
# loaded from disk, dyld has to slide each of them and rewrite all pointers (10 % of Foundation startup, plus
# copy-on-write page faults). With fixed addresses from ADDRESS upwards (default 40000000) this goes away as long as the
# range is free (otherwise dyld slides as before). In addition, thin i386 copies of the system libraries from /usr/lib
# go into the root (content as in the system, only rebased). Only files in intel/root are changed.
R=$(cd "$(dirname "$0")/.." && pwd)
W=$R/intel/root
A=${1:-40000000}
cd / || exit 1
n=0
for f in /usr/lib/*.dylib /usr/lib/system/*.dylib; do
  [ -h "$f" ] && continue                       # links (libfoo.dylib -> libfoo.1.dylib) not twice
  [ -f "$W$f" ] && continue
  lipo -info "$f" 2>/dev/null | grep -q i386 || continue
  mkdir -p "$W$(dirname "$f")"
  if lipo -info "$f" | grep -q 'Non-fat'; then cp -p "$f" "$W$f"; else lipo -thin i386 "$f" -output "$W$f"; fi && n=$((n + 1))
done
# recreate the links in /usr/lib (libSystem.dylib -> libSystem.B.dylib etc.) when the target is now in the root
for l in /usr/lib/*.dylib /usr/lib/system/*.dylib; do
  [ -h "$l" ] || continue
  [ -e "$W$l" ] || [ -h "$W$l" ] && continue
  target=$(readlink "$l")
  case "$target" in /*) z="$W$target" ;; *) z="$W$(dirname "$l")/$target" ;; esac
  [ -f "$z" ] && ln -s "$target" "$W$l"
done
echo "$n system libraries copied into the root as i386"
# all libraries (MH_DYLIB, i386 = header ce fa ed fe, type 6) of the root
find "$W/" -type f > /tmp/ir_all.txt     # (the slash: intel/root may be a symbolic link)
: > /tmp/ir_dylibs.txt
while read -r f; do
  k=$(head -c 16 "$f" 2>/dev/null | od -An -tx1 | tr -d ' \n')
  case "$k" in cefaedfe07000000????????06000000) echo "$f" >> /tmp/ir_dylibs.txt ;; esac
done < /tmp/ir_all.txt
grep -v ' ' /tmp/ir_dylibs.txt > /tmp/ir_dylibs2.txt     # skip paths with spaces (Spotlight importers)
chmod -R u+w "$W"                                         # copies from /usr/lib carry the system's permissions (r--)
echo "$(wc -l < /tmp/ir_dylibs2.txt) libraries, rebasing from 0x$A"
IFS='
'
rebase -low_address "$A" -arch i386 $(cat /tmp/ir_dylibs2.txt) 2>&1 | tail -5
# Prebinding only matches the addresses it was written with: redo it after every rebase (dyld checks only the
# timestamps, not the addresses)
sh $R/tools/intel_prebind.sh | tail -1
echo "done"
