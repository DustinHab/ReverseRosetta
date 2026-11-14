#!/bin/sh
# fonts_check.sh - (in the login session, through in_session.sh) font metrics: Intel version under rosetta against the
# PowerPC version. Prints "ok fonts" or the differences.
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
./fonts.ppc > /tmp/fonts_ppc.txt 2>&1
./rosetta ./fonts > /tmp/fonts_i386.txt 2>/tmp/fonts_err.txt
if cmp -s /tmp/fonts_ppc.txt /tmp/fonts_i386.txt; then
    echo "ok fonts ($(wc -l < /tmp/fonts_ppc.txt) lines)"
else
    echo "FAILED fonts"
    diff /tmp/fonts_ppc.txt /tmp/fonts_i386.txt | head -30
    head -5 /tmp/fonts_err.txt
fi
