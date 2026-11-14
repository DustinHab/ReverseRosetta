#!/bin/sh
# measure.sh [program] [rounds-small] [rounds-large] [repetitions] - translator speed: instructions per round from the
# interpreter, time per round from the slope between two round counts (startup cost drops out); plus native.
# Each run is measured W times and the fastest counts: disturbances (DirectoryService, Spotlight) only make runs
# slower, and a single slow start would otherwise distort the slope by milliseconds per round.
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
ulimit -t 600
P=${1:-bench_integer}; K=${2:-1}; G=${3:-31}; W=${4:-3}
insns() { ROSETTA_INTERP=1 ROSETTA_STATS=1 ./rosetta ./$P $1 2>&1 >/dev/null | sed -n 's/^rosetta: \([0-9]*\) instructions.*/\1/p'; }
# fastest of W run times of a command (output of the last run to $1)
fastest() {
    out=$1; shift
    perl -MTime::HiRes=time -e '
        my ($w, $out, @cmd) = @ARGV; my $min = 1e9;
        for (1 .. $w) { my $t = time; system("@cmd > $out 2>&1"); $t = time - $t; $min = $t if $t < $min; }
        print $min;' "$W" "$out" "$@"
}
b1=$(insns 1); b2=$(insns 2)
per=$((b2 - b1))
tk=$(fastest /tmp/m_k.txt ./rosetta ./$P $K)
tg=$(fastest /tmp/m_g.txt ./rosetta ./$P $G)
nk=$(fastest /dev/null ./$P.ppc $K)
ng=$(fastest /tmp/m_n.txt ./$P.ppc $G)
cmp -s /tmp/m_g.txt /tmp/m_n.txt && res=same || res=DIFFERENT
perl -e "printf \"%s: %d instructions per round, start %.3f s, per round %.2f ms = %.0f MIPS (native %.2f ms = %.0f MIPS, %.0f %%), result %s\n\",
    '$P', $per, $tk, ($tg - $tk) / ($G - $K) * 1000, $per * ($G - $K) / ($tg - $tk) / 1e6,
    ($ng - $nk) / ($G - $K) * 1000, $per * ($G - $K) / ($ng - $nk) / 1e6, 100 * ($ng - $nk) / ($tg - $tk), '$res'"
