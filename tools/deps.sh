#!/bin/sh
# deps.sh PATH... - all (transitive) dependencies of i386 images, resolved the way rosetta resolves them (Intel root
# first, then the system); reports what is missing as i386 and which symbols are needed from it
R=$(cd "$(dirname "$0")/.." && pwd)
W=${ROSETTA_INTEL:-$R/intel/root}
: > /tmp/dep_seen.txt
: > /tmp/dep_missing.txt
: > /tmp/dep_edges.txt
resolve() {     # install path -> file with an i386 part, or empty (STUBS_REBUILD=1: stub libraries count as missing)
    for k in "$W$1" "$1"; do
        [ -f "$k" ] && lipo -info "$k" 2>/dev/null | grep -q i386 || continue
        [ -n "$STUBS_REBUILD" ] && grep -q -e "rosetta-stub" -e "rosetta-Platzhalter" "$k" && continue   # (older marker)
        echo "$k"; return
    done
}
visit() {
    p=$1
    grep -qxF "$p" /tmp/dep_seen.txt && return
    echo "$p" >> /tmp/dep_seen.txt
    f=$(resolve "$p")
    if [ -z "$f" ]; then echo "$p" >> /tmp/dep_missing.txt; return; fi
    # (install names never contain spaces, client paths can: "Activity Monitor.app"; the edges are tab-separated)
    otool -arch i386 -L "$f" 2>/dev/null | tail -n +2 | awk '{print $1}' | grep -vxF "$p" | grep -v "^@" > /tmp/dep_d.$$
    while read d; do printf '%s\t%s\n' "$p" "$d" >> /tmp/dep_edges.txt; done < /tmp/dep_d.$$
    for d in $(cat /tmp/dep_d.$$); do visit "$d"; done
}
for p in "$@"; do visit "$p"; done
rm -f /tmp/dep_d.$$
echo "$(wc -l < /tmp/dep_seen.txt | tr -d ' ') images, $(wc -l < /tmp/dep_missing.txt | tr -d ' ') missing:"
while read m; do
    n=$(basename "$m" | sed 's/\.dylib$//; s/\..*//')
    echo "== $m"
    grep "	$m\$" /tmp/dep_edges.txt | cut -f1 | while IFS= read -r v; do
        f=$(resolve "$v")
        c=$(nm -arch i386 -m "$f" 2>/dev/null | grep undefined | grep -c "(from $n)")
        echo "   from $(basename "$v"): $c symbols"
    done
done < /tmp/dep_missing.txt
