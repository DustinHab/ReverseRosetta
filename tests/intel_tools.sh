#!/bin/sh
# intel_tools.sh - real Intel programs from the system (universal binaries) under rosetta against native:
# same output, time (fastest of 3) native and under rosetta
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
ulimit -t 120
z() { perl -MTime::HiRes=time -e 'my $m = 1e9; for (1..3) { my $t = time; system("@ARGV > /dev/null 2>&1"); $t = time - $t; $m = $t if $t < $m } printf "%.0f", $m * 1000' "$@"; }
probe() {
  b=$1; shift
  lipo -info "$b" 2>/dev/null | grep -q i386 || { echo "$b: no i386"; return; }
  "$b" "$@" > /tmp/fr_n.txt 2>&1; rn=$?
  ./rosetta "$b" "$@" > /tmp/fr_r.txt 2>&1; rr=$?
  if cmp -s /tmp/fr_n.txt /tmp/fr_r.txt && [ $rn = $rr ]; then g=same; else g="DIFFERENT (exit status $rn/$rr)"; fi
  echo "$b $*: $g, native $(z "$b" "$@") ms, rosetta $(z ./rosetta "$b" "$@") ms"
}
probe /bin/cat /etc/hosts
probe /usr/bin/wc -l /usr/share/dict/words
probe /usr/bin/sort /etc/services
probe /usr/bin/grep -c tcp /etc/services
probe /usr/bin/awk -F: '{ n += NF } END { print n }' /etc/passwd
probe /usr/bin/sed -e s/a/b/g /etc/services
probe /usr/bin/find /usr/share/man/man5 -name "*.5"
probe /usr/bin/bc -v
probe /usr/bin/perl -e 'print 2**40, "\n"'
probe /usr/bin/python -c 'print sum(range(100000))'
probe /usr/bin/ruby -e 'puts (1..1000).inject(:+)'
probe /usr/bin/tclsh /dev/null
