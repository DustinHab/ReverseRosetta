#!/bin/sh
# guest_sym.sh ADDR... - map a guest address (i386, rebased libraries of the Intel root) to library and function
R=$(cd "$(dirname "$0")/.." && pwd)
W=$R/intel/root
[ -f /tmp/gs_text.txt ] || {
  find $W -type f -name "*.dylib" > /tmp/gs_l.txt
  find $W/System/Library/Frameworks $W/System/Library/PrivateFrameworks -type f -perm -100 2>/dev/null | grep -v "\.dylib$" >> /tmp/gs_l.txt
  while read -r f; do
    otool -arch i386 -l "$f" 2>/dev/null | awk -v f="$f" '/segname __TEXT/ { getline; a = $2; getline; print a, $2, f; exit }'
  done < /tmp/gs_l.txt > /tmp/gs_text.txt
}
for x in "$@"; do
  perl -e '$x = hex shift; open T, "/tmp/gs_text.txt"; while (<T>) { ($a, $g, $f) = split " ", $_, 3; chomp $f;
    if ($x >= hex($a) && $x < hex($a) + hex($g)) { print "$f\n"; exit } }' "$x" > /tmp/gs_f.txt
  f=$(cat /tmp/gs_f.txt)
  if [ -z "$f" ]; then echo "$x: no image"; continue; fi
  nm -arch i386 -n "$f" | perl -e '$x = hex shift; while (<STDIN>) { @f = split; next unless @f == 3; $v = hex $f[0]; last if $v > $x; ($b, $bv) = ($f[2], $v) }
    printf "%s = %s+0x%x\n", $ARGV[0], $b, $x - $bv' "$x" "$x"
  echo "    in $f"
done
