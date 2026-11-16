#!/bin/sh
# app_stubs.sh APP... - stub libraries for application bundles that stay where they are installed (universal
# programs in /Applications run their i386 part through rosetta): as install_app.sh does for intel/apps, the
# libraries that the i386 executables of the bundle need and that are missing as i386 get stub libraries, and existing
# stub libraries get the symbols these programs import (private frameworks such as GraphKit are not in the SDK, so
# sdk_stubs_all.sh cannot complete them).
R=$(cd "$(dirname "$0")/.." && pwd)
# all i386 executables of all bundles at once: intel_stubs.sh builds each stub library one time (with the symbols that
# all these programs import), instead of once per program
: > /tmp/app_stubs_paths.txt
for A in "$@"; do
    find "${A%/}/Contents" -type f -perm -u+x 2>/dev/null | while read -r f; do
        lipo -info "$f" 2>/dev/null | grep -q i386 && echo "$f"
    done >> /tmp/app_stubs_paths.txt
done
echo "$(wc -l < /tmp/app_stubs_paths.txt | tr -d ' ') i386 executables in $# bundles"
[ -s /tmp/app_stubs_paths.txt ] || exit 0
IFS='
'
STUBS_REBUILD=1 sh "$R/tools/intel_stubs.sh" $(cat /tmp/app_stubs_paths.txt) 2>&1
unset IFS
sh "$R/tools/stubs_rebuild_all.sh"          # kinds from the libraries themselves, classes
