#!/bin/sh
# guest_profile.sh ["PROG ARGS"] [RUNS] - sampling profile on guest functions: where the Intel program runs (in
# translated code: block eip; when interpreting: Cpu.eip), resolved through the i386 symbols of the images (Intel
# root before system). Plus the share of host work (translation, dispatcher, system calls). GP_BLOCKS=1: per block
# (image + offset of the block start) instead of per function.
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R/build || exit 1
P=${1:-./hello}; N=${2:-10}
W=$R/intel/root
# GP_KEEP=1: evaluate /tmp/gp.txt as recorded (ROSETTA_HPROFILE=/tmp/gp.txt with arguments the line above cannot pass)
if [ -z "$GP_KEEP" ]; then
    rm -f /tmp/gp.txt /tmp/gp.txt.guest
    i=0
    while [ $i -lt $N ]; do ROSETTA_HPROFILE=/tmp/gp.txt ./rosetta $P > /dev/null 2>&1; i=$((i + 1)); done
fi
nm -n rosetta | grep -i ' t ' > /tmp/gp_sym.txt
# images with hits: read the symbols (i386) of each image
cut -d' ' -f1,2 /tmp/gp.txt.guest | tr ' ' '\n' | grep '+' | sed 's/+[0-9a-f]*$//' | sort -u > /tmp/gp_images.txt
rm -f /tmp/gp_bsym.txt
while read -r b; do
  f="$b"; [ -f "$W$b" ] && f="$W$b"
  case "$b" in /*) ;; *) f="$b" ;; esac
  [ -f "$f" ] || continue
  t=$(otool -arch i386 -l "$f" 2>/dev/null | awk '/segname __TEXT/ { getline; print $2; exit }')
  nm -arch i386 -n "$f" 2>/dev/null | perl -ne 'BEGIN { ($b, $t) = @ARGV; @ARGV = (); $t = hex $t }
      @f = split; printf "%s %x %s\n", $b, hex($f[0]) - $t, $f[2] if @f == 3 && $f[1] =~ /^[tT]$/' "$b" "${t:-0}" >> /tmp/gp_bsym.txt
done < /tmp/gp_images.txt
perl -e '
  open S, "/tmp/gp_sym.txt"; while (<S>) { @f = split; push @a, hex $f[0]; push @n, $f[2]; } close S;
  sub hfn { my $x = shift; return "[Libraries]" if $x >= 0x90000000; return "[Guest]" if $x < $a[0];
    my ($lo, $hi) = (0, $#a); while ($lo < $hi) { my $m = int(($lo + $hi + 1) / 2); if ($a[$m] <= $x) { $lo = $m } else { $hi = $m - 1 } }
    return ($x - $a[$lo] > 0x40000) ? "[Code cache]" : $n[$lo]; }
  open B, "/tmp/gp_bsym.txt"; while (<B>) { my ($b, $o, $s) = split; push @{$bo{$b}}, hex $o; push @{$bs{$b}}, $s; } close B;
  sub gfn { my $g = shift; return "?" unless $g =~ /^(.*)\+([0-9a-f]+)$/; my ($b, $o) = ($1, hex $2);
    my $short = $b; $short =~ s{.*/}{};
    my $ao = $bo{$b}; return sprintf("%s+%x (no symbols: block)", $short, $o) unless $ao && @$ao;
    return sprintf("%s+%x", $short, $o) if $ENV{GP_BLOCKS};      # GP_BLOCKS=1: the blocks themselves
    my ($lo, $hi) = (0, $#$ao); return "$short+?" if $o < $ao->[0];
    while ($lo < $hi) { my $m = int(($lo + $hi + 1) / 2); if ($ao->[$m] <= $o) { $lo = $m } else { $hi = $m - 1 } }
    return "$short:" . $bs{$b}[$lo]; }
  open P, "/tmp/gp.txt"; open G, "/tmp/gp.txt.guest";
  while (defined(my $l = <P>)) { my $g = <G>; my @z = split " ", $l; my ($w, $e) = split " ", $g; $t++;
    my $h = hfn(hex $z[0]);
    my $k;
    if ($h eq "[Code cache]") { $k = "translated " . gfn($w); $uc++ }
    elsif ($h =~ /step|decode|cold_run|_alu$|rm_read|rm_write|condition|flags_fetch|sse_|x87_|fpu/) { $k = "interpreted " . gfn($e); $ui++ }
    elsif ($h =~ /^\[Libraries\]$/) { $k = "Host: libraries (system calls)"; }
    else { $k = "Host: $h"; $uh++ }
    $c{$k}++; }
  printf "%d samples: translated guest code %.1f%%, interpreted %.1f%%, rest host\n", $t, 100 * $uc / $t, 100 * $ui / $t;
  my $n = 0; for (sort { $c{$b} <=> $c{$a} } keys %c) { printf "%5.1f%% %s\n", 100 * $c{$_} / $t, $_; last if ++$n >= ($ENV{Z} || 40) }
'
