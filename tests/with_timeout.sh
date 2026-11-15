#!/bin/sh
# with_timeout.sh SECONDS COMMAND... - command with a wall-clock timeout (ulimit -t counts only CPU time; waiting runs hang)
s=$1; shift
perl -e 'my $s = shift; my $pid = fork; if (!$pid) { exec @ARGV; exit 127 } local $SIG{ALRM} = sub { kill 9, $pid; print STDERR "TIMEOUT expired\n"; exit 124 }; alarm $s; waitpid $pid, 0; exit($? & 127 ? 128 + ($? & 127) : $? >> 8)' "$s" "$@"
