#!/bin/sh
# app_deps.sh NAME - missing i386 libraries of a program in intel/apps/NAME.app
# (main executable, embedded frameworks, plug-ins): collect the system paths of all images, then deps.sh
R=$(cd "$(dirname "$0")/.." && pwd)
Z=$R/intel/apps/$1.app
find "$Z/Contents" -type f -perm -u+x 2>/dev/null | while read f; do
    lipo -info "$f" 2>/dev/null | grep -q i386 && otool -arch i386 -L "$f" | tail -n +2 | awk '{ print $1 }'
done | grep -v "^@" | sort -u > /tmp/pa_paths.txt
echo "$(wc -l < /tmp/pa_paths.txt | tr -d ' ') direct system paths"
sh "$(dirname "$0")/deps.sh" $(cat /tmp/pa_paths.txt)
