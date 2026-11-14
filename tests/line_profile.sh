#!/bin/sh
# line_profile.sh [PROG] [RUNS] [N] - sampling profile on source lines: builds build/rosetta_g (-g, same optimization),
# measures like profile.sh and maps the hits in the translator to lines with gdb (the N most frequent)
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R || exit 1
P=${1:-hello}; L=${2:-20}; N=${3:-60}
CF="-arch ppc -mcpu=7450 -O3 -mdynamic-no-pic -Wall -Wno-unused-function -std=gnu99 -g"
mkdir -p build/g
for f in main macho decoder interp fpu sys mach_msg commpage sse thunk jit thread; do
  [ build/g/$f.o -nt $f.c ] && [ build/g/$f.o -nt jit.h ] && [ build/g/$f.o -nt cpu.h ] || gcc-4.2 $CF -c -o build/g/$f.o $f.c || exit 1
done
gcc-4.2 -arch ppc -o build/rosetta_g build/g/*.o -Wl,-pagezero_size,0x10000000 || exit 1
cd build
rm -f /tmp/profz.txt
i=0
while [ $i -lt $L ]; do ROSETTA_HPROFILE=/tmp/profz.txt ./rosetta_g ./$P > /dev/null 2>&1; i=$((i + 1)); done
T=$(wc -l < /tmp/profz.txt)
# only hits in the program itself (below the code cache), counted per address
FROM=$(nm -n rosetta_g | grep -i ' t ' | head -1 | cut -c1-8); TO=$(nm -n rosetta_g | grep -i ' t ' | tail -1 | cut -c1-8)
awk '{ print $1 }' /tmp/profz.txt | sort | uniq -c | sort -rn | awk -v v=$FROM -v b=$TO '$2 >= v && $2 <= b' > /tmp/profz_adr.txt
awk '{ print "info line *0x" $2 }' /tmp/profz_adr.txt > /tmp/profz_gdb.txt
gdb -batch -x /tmp/profz_gdb.txt ./rosetta_g 2>/dev/null | grep -E '^(Line|No line)' > /tmp/profz_lines.txt
paste -d' ' /tmp/profz_adr.txt /tmp/profz_lines.txt | perl -ne '
  ($n) = /^\s*(\d+)/; if (/Line (\d+) of "([^"]+)"/) { $z = "$2:$1" } else { $z = "?" }
  if (/<(\w+)\+/) { $f{$z} = $1 } $c{$z} += $n; END {
  for (sort { $c{$b} <=> $c{$a} } keys %c) { printf "%6.2f%%  %-14s %s\n", 100 * $c{$_} / '"$T"', $_, $f{$_}; last if ++$k >= '"$N"' } }'
echo "($T samples)"
