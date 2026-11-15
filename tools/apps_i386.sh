#!/bin/sh
# apps_i386.sh [DIR...] - application bundles whose executable has an i386 part (default: /Applications, its
# subfolders, /Developer/Applications), one path per line; with -a also the others, with their architectures
all=0
[ "$1" = -a ] && { all=1; shift; }
[ $# -gt 0 ] || set -- /Applications /Developer/Applications
for d in "$@"; do
    find "$d" -maxdepth 3 -name '*.app' -type d -prune 2>/dev/null
done | sort | while read -r a; do
    e=$(defaults read "$a/Contents/Info" CFBundleExecutable 2>/dev/null)
    f="$a/Contents/MacOS/$e"
    [ -f "$f" ] || continue
    archs=$(lipo -info "$f" 2>/dev/null | sed 's/.*: //')
    case "$archs" in
        *i386*) if [ $all = 1 ]; then echo "$a	$archs"; else echo "$a"; fi ;;
        *) [ $all = 1 ] && echo "$a	$archs" ;;
    esac
done
