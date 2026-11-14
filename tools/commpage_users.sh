#!/bin/sh
# commpage_users.sh [LIBRARY] - which functions of the i386 libSystem use which commpage address (0xffff....).
# Output: address kind function
L=${1:-/usr/lib/libSystem.B.dylib}
otool -arch i386 -tV "$L" > /tmp/ls_i386.dis
awk '
/^[_A-Za-z$].*:$/ { f = $0; next }
/0xffff[0-9a-f][0-9a-f][0-9a-f][0-9a-f]/ {
  match($0, /0xffff[0-9a-f][0-9a-f][0-9a-f][0-9a-f]/); a = substr($0, RSTART, RLENGTH)
  kind = ($2 ~ /^(call|jmp)/) ? $2 : "data:" $2
  print a, kind, f
}' /tmp/ls_i386.dis | sort | uniq -c | sort -k2
