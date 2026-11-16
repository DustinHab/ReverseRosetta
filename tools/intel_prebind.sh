#!/bin/sh
# intel_prebind.sh [-n] - prebind the i386 libraries of the Intel root (tools/prebind.c), after intel_rebase.sh.
# Once beforehand, a backup of the whole root goes to intel/root_before_prebind.tar (restore: tar xf ... -C intel).
# -n: check only. Only files in intel/root are changed.
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R || exit 1
W=$R/intel/root
gcc -O2 -Wall -o build/prebind tools/prebind.c || exit 1
find "$W/" -type f > /tmp/vb_all.txt     # (the slash: intel/root may be a symbolic link)
: > /tmp/vb_dylibs.txt
while read -r f; do
  k=$(head -c 16 "$f" 2>/dev/null | od -An -tx1 | tr -d ' \n')
  case "$k" in cefaedfe07000000????????06000000) echo "$f" >> /tmp/vb_dylibs.txt ;; esac
done < /tmp/vb_all.txt
if [ "$1" != -n ] && [ ! -f intel/root_before_prebind.tar ]; then
  echo "backup to intel/root_before_prebind.tar"
  tar cf intel/root_before_prebind.tar -C intel root || exit 1
fi
build/prebind "$W" /tmp/vb_dylibs.txt $1
