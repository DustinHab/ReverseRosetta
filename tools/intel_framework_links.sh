#!/bin/sh
# intel_framework_links.sh - every framework bundle of the Intel root (nested ones too) whose binary exists in exactly
# one version (Versions/A/NAME, Foundation's and AppKit's Versions/C, Python's 2.5 ...) gets the links Versions/Current ->
# that version and NAME -> Versions/Current/NAME where they are missing (QuickTime Player dlopens Foundation's short path):
# dlopen and NSBundle use the short path (ImageKit.framework/ImageKit); without the link the path redirection does not
# find the i386 file and the guest gets the PowerPC one of the system ("no matching architecture"). Also removes
# links of that kind that lead nowhere. Only intel/root is changed.
R=$(cd "$(dirname "$0")/.." && pwd)
W=${ROSETTA_INTEL:-$R/intel/root}
cd "$W" || exit 1
made=0; gone=0
find . -type l -name '*' 2>/dev/null | while read l; do
    case "$(readlink "$l")" in Versions/Current/*) [ -e "$l" ] || { rm "$l"; echo "removed $l (led nowhere)"; } ;; esac
done
find . -type d -name '*.framework' 2>/dev/null | while read fw; do
    n=$(basename "$fw" .framework)
    v=$(cd "$fw/Versions" 2>/dev/null && for x in *; do [ "$x" != Current ] && [ -f "$x/$n" ] && echo "$x"; done)
    [ -n "$v" ] && [ $(echo "$v" | wc -l) -eq 1 ] || continue
    ( cd "$fw" || exit
      [ -e Versions/Current ] || { ln -sfh "$v" Versions/Current; echo "linked ${fw#./}/Versions/Current -> $v"; }
      [ -e "$n" ] || { ln -sfh "Versions/Current/$n" "$n"; echo "linked ${fw#./}/$n"; } )
done
