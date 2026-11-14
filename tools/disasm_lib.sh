#!/bin/sh
# disasm_lib.sh LIBRARY FUNCTION [LINES] - i386 code of a function (otool), starting at its symbol
B=$1; F=$2; Z=${3:-80}
otool -arch i386 -tV "$B" | awk -v f="$F:" -v z="$Z" '$0 == f { p = 1; n = 0 } p { print; n++ } n > z { exit }'
