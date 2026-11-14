#!/bin/sh
# walltime.sh COMMAND... - wall-clock time and status of a command (output discarded)
perl -MTime::HiRes=time -e '$t = time; system("@ARGV > /dev/null 2>&1"); printf "wall time %.0f ms, status %d\n", (time - $t) * 1000, $? >> 8' "$@"
