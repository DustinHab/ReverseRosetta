#!/bin/sh
# imports_from.sh INSTALL_PATH - which symbols the i386 images (after deps.sh: /tmp/dep_edges.txt) take from this
# library, with binding kind (lazy = function at its first call, nonlazy = at load time: data or function pointers);
# also via umbrella names (Carbon, ApplicationServices ...) that re-export the library
R=$(cd "$(dirname "$0")/.." && pwd)
W=${ROSETTA_INTEL:-$R/intel/root}
n=$(basename "$1" | sed 's/\.dylib$//; s/\..*//')
for f in $(awk '{print $1}' /tmp/dep_edges.txt | sort -u); do
    [ -f "$W$f" ] && g="$W$f" || g="$f"
    nm -arch i386 -m "$g" 2>/dev/null | grep undefined | grep "(from $n)" |
        awk '{ kind = ($2 == "[lazy") ? "lazy" : "nonlazy"; print kind, $(NF-2) }'
done | sort -u
