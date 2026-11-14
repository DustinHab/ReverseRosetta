#!/bin/sh
# show_block_version.sh EIP [N] - disassemble the N-th version (default: the last) of a block from /tmp/rosetta_blocks.s
# (blocks are translated again, variants have the same address)
cd /tmp || exit 1
perl -e '$e = hex(shift); $w = shift; open F, "/tmp/rosetta_blocks.s"; $k = 0; @f = ();
  while (<F>) { if (/^b_(\w+):/) { $p = hex($1) == $e; if ($p) { $k++; $f[$k] = ""; } } $f[$k] .= $_ if $p; }
  $w = $k if !$w || $w > $k; print $f[$w];' "$1" "$2" > blk.s
grep "^;" blk.s | tr "\n" " "; echo
grep -v "^;" blk.s > blk2.s
as -arch ppc -o blk.o blk2.s < /dev/null && otool -tv blk.o | tail -n +3
