#!/bin/sh
# profile.sh [PROG] [RUNS] [BIN] - sampling profile of the translator (SIGALRM every 100 us)
# List 1: self (where the processor was); list 2: inclusive (function anywhere in the call chain).
# Libraries are resolved through the symbols of the PowerPC libSystem (L:name), the commpage as [Commpage].
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
P=${1:-./hello}; N=${2:-20}; B=${3:-rosetta}
case "$P" in /*|./*) ;; *) P=./$P ;; esac
rm -f /tmp/prof.txt
i=0
while [ $i -lt $N ]; do ROSETTA_HPROFILE=/tmp/prof.txt ./$B $P > /dev/null 2>&1; i=$((i + 1)); done
nm -n $B | grep -i ' t ' > /tmp/prof_sym.txt
nm -arch ppc -n /usr/lib/libSystem.B.dylib 2>/dev/null | grep -i ' t ' > /tmp/prof_lib.txt
perl -e '
  sub load { my ($d, $a, $n) = @_; open S, $d; while (<S>) { @f = split; push @$a, hex $f[0]; push @$n, $f[2]; } close S; }
  load("/tmp/prof_sym.txt", \@a, \@n); load("/tmp/prof_lib.txt", \@la, \@ln);
  sub lookup { my ($x, $a) = @_; my ($lo, $hi) = (0, $#$a); return -1 if $hi < 0 || $x < $a->[0];
    while ($lo < $hi) { my $m = int(($lo + $hi + 1) / 2); if ($a->[$m] <= $x) { $lo = $m } else { $hi = $m - 1 } } return $lo; }
  sub fn { my $x = shift;
    return "[Commpage]" if $x >= 0xffff0000;
    if ($x >= 0x90000000) { my $i = lookup($x, \@la); return ($i >= 0 && $x - $la[$i] < 0x10000) ? "L:$ln[$i]" : "[Libraries]"; }
    return "[Guest]" if $x < $a[0];
    my $i = lookup($x, \@a); return ($x - $a[$i] > 0x40000) ? "[Code cache]" : $n[$i]; }
  open P, "/tmp/prof.txt"; while (<P>) { @z = split; $t++; $s = fn(hex $z[0]); $c{$s}++;
    %d = (); for $x (@z) { $d{fn(hex $x)} = 1 } for (keys %d) { $incl{$_}++ } }
  printf "%d samples\n%-34s %-34s\n", $t, "self", "inclusive";
  @s = sort { $c{$b} <=> $c{$a} } keys %c; @i = sort { $incl{$b} <=> $incl{$a} } keys %incl;
  for $k (0..($ENV{Z} || 44)) { last if $k > $#s && $k > $#i;
    printf "%5.1f%% %-27s %5.1f%% %s\n", $k <= $#s ? 100 * $c{$s[$k]} / $t : 0, $s[$k], $k <= $#i ? 100 * $incl{$i[$k]} / $t : 0, $i[$k] }
'
