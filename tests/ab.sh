#!/bin/sh
# ab.sh "ENV A" "ENV B" ["PROG ARGS"] [N] - run two settings alternately (same conditions),
# minimum and median of the run time per setting. Example: sh ab.sh "ROSETTA_THRESHOLD=15" "ROSETTA_THRESHOLD=15 ROSETTA_NO_CACHE=1"
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
A=$1; B=$2; P=${3:-./hello}; N=${4:-15}
perl -MTime::HiRes=time -e '
  my ($ea, $eb, $p, $n) = @ARGV; my (@ta, @tb);
  for (1 .. $n) {
    for my $s ([$ea, \@ta], [$eb, \@tb]) {
      my $x = $s->[0] =~ s/\bBIN=(\S+)\s*// ? $1 : "rosetta";     # BIN=name selects another program in build/
      my $t = time; system("env $s->[0] ./$x $p > /dev/null 2>&1"); push @{$s->[1]}, (time - $t) * 1000;
    }
  }
  for my $s (["A", \@ta, $ea], ["B", \@tb, $eb]) {
    my @x = sort { $a <=> $b } @{$s->[1]};
    printf "%s: min %.1f ms, median %.1f ms  (%s)\n", $s->[0], $x[0], $x[int(@x / 2)], $s->[2];
  }' "$A" "$B" "$P" "$N"
