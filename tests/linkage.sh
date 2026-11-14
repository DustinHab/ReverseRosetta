#!/bin/sh
# linkage.sh PROG - which libraries PROG (i386 and .ppc) is linked against
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
otool -L ./$1 ./$1.ppc
