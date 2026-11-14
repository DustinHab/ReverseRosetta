#!/bin/sh
# missing_imports.sh LIBRARY_NAME... - which symbols the i386 images of the Intel root import from these (missing)
# libraries (two-level namespace: nm -m shows "(from NAME)")
R=$(cd "$(dirname "$0")/.." && pwd)
W=$R/intel/root
find $W -type f \( -perm -u+x -o -name "*.dylib" \) 2>/dev/null | while read f; do
    file "$f" 2>/dev/null | grep -q "Mach-O" || continue
    for n in "$@"; do
        nm -m "$f" 2>/dev/null | grep "undefined" | grep "(from $n)" | awk -v d="$f" -v n="$n" '{print n, $(NF-2), d}'
    done
done | sed "s|$W||" | sort -u > /tmp/missing_imports.txt
for n in "$@"; do
    echo "== $n: $(grep -c "^$n " /tmp/missing_imports.txt) references, $(grep "^$n " /tmp/missing_imports.txt | awk '{print $2}' | sort -u | wc -l | tr -d ' ') symbols"
    grep "^$n " /tmp/missing_imports.txt | awk '{print $3}' | sort | uniq -c | sort -rn | head -8
done
