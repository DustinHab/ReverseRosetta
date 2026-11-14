#!/bin/sh
# profile_callers.sh PATTERN - from the last profile (/tmp/prof.txt from profile.sh): from which translator functions the
# processor was in addresses whose name matches PATTERN (e.g. Commpage, Libraries, map_add)
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
M=${1:-Commpage}; B=${2:-rosetta}
perl -e '
  sub load { my ($d, $a, $n) = @_; open S, $d; while (<S>) { @f = split; push @$a, hex $f[0]; push @$n, $f[2]; } close S; }
  load("/tmp/prof_sym.txt", \@a, \@n);
  sub lookup { my ($x, $a) = @_; my ($lo, $hi) = (0, $#$a); return -1 if $hi < 0 || $x < $a->[0];
    while ($lo < $hi) { my $m = int(($lo + $hi + 1) / 2); if ($a->[$m] <= $x) { $lo = $m } else { $hi = $m - 1 } } return $lo; }
  sub fn { my $x = shift; return "[Commpage]" if $x >= 0xffff0000; return "[Libraries]" if $x >= 0x90000000;
    return "[Guest]" if $x < $a[0]; my $i = lookup($x, \@a); return ($x - $a[$i] > 0x40000) ? "[Code cache]" : $n[$i]; }
  open P, "/tmp/prof.txt"; while (<P>) { @z = split; $t++; next unless fn(hex $z[0]) =~ /$ARGV[0]/; $m++;
    my @k = map { fn(hex $_) } @z[1..$#z]; my @w = grep { !/^\[/ && !/$ARGV[0]/ } @k;
    $c{join(" < ", @w[0..($#w < 2 ? $#w : 2)])}++; }
  printf "%d of %d samples in %s; callers (up to 3 levels):\n", $m, $t, $ARGV[0];
  for (sort { $c{$b} <=> $c{$a} } keys %c) { printf "%5.1f%% %s\n", 100 * $c{$_} / $t, $_; last if ++$k >= 25 }
' "$M"
