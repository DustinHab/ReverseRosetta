#!/bin/sh
# vdsp_check.sh [ROUNDS] - build vdsp_d (Intel libvDSP/libvMisc), compare translated against interpreted, times
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R || exit 1
S=/Developer/SDKs/MacOSX10.5.sdk
V=$S/System/Library/Frameworks/Accelerate.framework/Versions/A/Frameworks/vecLib.framework/Versions/A
[ build/vdsp_d -nt tests/vdsp_d.c ] || gcc-4.2 -arch i386 -O2 -isysroot $S -o build/vdsp_d tests/vdsp_d.c $V/libvDSP.dylib $V/libvMisc.dylib || exit 1
cd build
ulimit -t 300
# reference: everything interpreted, but with the same native math functions (thunk.c) as when translating; the pure
# interpreter computes sin/cos with the Intel code and differs in the last bit (FFT twiddle factors)
ROSETTA_NO_CACHE=1 ROSETTA_THRESHOLD=2 ROSETTA_ONLY=0-0 ./rosetta ./vdsp_d > /tmp/vd_i.txt 2>&1
failed=0
for v in "ROSETTA_X=1" "ROSETTA_THRESHOLD=1" "ROSETTA_THRESHOLD=1 ROSETTA_NO_XFH=1" "ROSETTA_NO_PD=1"; do
    env $v ./rosetta ./vdsp_d > /tmp/vd_x.txt 2>&1
    if cmp -s /tmp/vd_x.txt /tmp/vd_i.txt; then echo "ok      $v"; else echo "FAILED  $v"; diff /tmp/vd_i.txt /tmp/vd_x.txt | head -10; failed=1; fi
done
tail -1 /tmp/vd_i.txt
R=${1:-200}
for v in "ROSETTA_X=1" "ROSETTA_NO_PD=1"; do
    env $v ./rosetta ./vdsp_d 1 > /dev/null
    perl -MTime::HiRes=time -e '$t = time; system("env @ARGV > /dev/null"); printf "%-20s %d rounds: %.2f s\n", $ARGV[0], $ARGV[3], time - $t' $v ./rosetta ./vdsp_d $R
done
exit $failed
