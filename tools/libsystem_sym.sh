#!/bin/sh
# libsystem_sym.sh KNOWN_SYMBOL KNOWN_ADDR ADDR... - nearest symbol (locals too) of the PowerPC libSystem at or below
# an address in the shared region; the slide relative to the file comes from a known pair (symbol, address), e.g.
# _mach_msg_trap 902d5070
nm -arch ppc -n /usr/lib/libSystem.B.dylib 2>/dev/null | grep -i -E ' [tT] ' > /tmp/ls_sym.txt
b=$(awk -v s="$1" '$3 == s {print $1}' /tmp/ls_sym.txt | head -1)
k=$2; shift 2
for a in "$@"; do
    perl -e '($b, $k, $x) = map { hex } @ARGV; $d = $x - ($k - $b); open S, "/tmp/ls_sym.txt";
             while (<S>) { @f = split; $v = hex $f[0]; last if $v > $d; $n = $f[2]; $nv = $v }
             printf "%08x = %s+0x%x\n", $x, $n, $d - $nv' $b $k $a
done
