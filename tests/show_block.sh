#!/bin/sh
# show_block.sh EIP - disassemble a block from /tmp/rosetta_blocks.s (ROSETTA_SHOW), with the Intel addresses
cd /tmp || exit 1
perl -e '$e = hex(shift); open F, "/tmp/rosetta_blocks.s"; while (<F>) { if (/^b_(\w+):/) { $p = hex($1) == $e; } print if $p; }' "$1" > blk.s
grep "^;" blk.s | tr "\n" " "; echo
grep -v "^;" blk.s > blk2.s
as -arch ppc -o blk.o blk2.s < /dev/null && otool -tv blk.o | tail -n +3
