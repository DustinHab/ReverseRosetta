#!/bin/sh
# nm_names.sh LIBRARY NAME... - i386 symbol addresses (offset in the image) for the given names
B=$1; shift
nm -arch i386 "$B" > /tmp/nm_names.txt
for n in "$@"; do grep " $n\$" /tmp/nm_names.txt; done
