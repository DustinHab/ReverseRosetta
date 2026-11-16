#!/bin/sh
# stubs_rebuild_all.sh - rebuild every stub library of the Intel root with sdk_stub.sh: each keeps its symbols, gets
# the kind of each symbol from the SDK or from the PowerPC library of the system (function or data), every symbol of
# the SDK and real Objective-C classes with the right superclass (stub_classes.sh). After intel_stubs.sh, whose
# symbol kinds come from how programs import them.
R=$(cd "$(dirname "$0")/.." && pwd)
W=${ROSETTA_INTEL:-$R/intel/root}
cd "$W" || exit 1
find . -type f \( -path '*/Versions/*' -o -name '*.dylib' \) 2>/dev/null | while IFS= read -r f; do
    grep -q -e rosetta-stub -e rosetta-Platzhalter "$f" 2>/dev/null && echo "${f#.}"
done | while IFS= read -r m; do sh "$R/tools/sdk_stub.sh" "$m"; done