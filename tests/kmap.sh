#!/bin/sh
# kmap.sh [COMMAND...] - used entries of the kernel zone "kernel map entries" before and after a command
z() { zprint | awk '/^kernel.map.entries/ { print $7 }'; }
a=$(z)
if [ $# -gt 0 ]; then "$@"; fi
b=$(z)
echo "kernel map entries: before $a, after $b, increase $((b - a))"
