#!/bin/sh
# gl_check.sh - (in the login session, through in_session.sh) OpenGL: every workload of bench_gl translated and
# native must render the same image (checksum of the pixels). Prints "ok gl ..." or the differences.
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
./bench_gl.ppc 1 > /tmp/glc_ppc.txt 2>&1
./rosetta ./bench_gl 1 > /tmp/glc_i386.txt 2>/tmp/glc_err.txt
grep image /tmp/glc_ppc.txt | sed 's/ .*image / /' > /tmp/glc_a.txt
grep image /tmp/glc_i386.txt | sed 's/ .*image / /' > /tmp/glc_b.txt
n=$(wc -l < /tmp/glc_a.txt | tr -d ' ')
if [ "$n" -gt 0 ] && cmp -s /tmp/glc_a.txt /tmp/glc_b.txt; then
    echo "ok gl ($n workloads, $(head -1 /tmp/glc_ppc.txt | sed 's/renderer //'))"
else
    echo "FAILED gl"
    paste -d '|' /tmp/glc_ppc.txt /tmp/glc_i386.txt | head -20
    head -5 /tmp/glc_err.txt
fi
