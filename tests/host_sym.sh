#!/bin/sh
# host_sym.sh ADDRESS... - function in rosetta for an address (also static functions)
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
nm -n rosetta | perl -e '
    my @a = map { hex } @ARGV; my @s;
    while (<STDIN>) { push @s, [hex($1), $2] if /^([0-9a-f]+) [tT] (\S+)/; }
    for my $x (@a) { my $b = ""; for (@s) { last if $_->[0] > $x; $b = sprintf "%s+%d", $_->[1], $x - $_->[0]; } printf "%08x %s\n", $x, $b; }
' "$@"
