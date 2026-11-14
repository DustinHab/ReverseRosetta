#!/usr/bin/perl
# ats_layout.pl DISASSEMBLY [FUNCTION...] - draft of the field layouts of the ATS messages from Intel's client code.
# For each function that calls SendFontManagementMessage(type, buffer, ...): type (constant in the first argument),
# buffer (leal X(%ebp) as second argument), then all accesses disp(%ebp) in the buffer relative to its start - before
# the send (request) and after it (reply) - with width (4: movl, 2: movw/movzwl/movswl, 1: movb/movzbl), "addr" for
# leal (embedded structures, strings). Only a draft: it does not follow pointers into other registers.
use strict;
my $file = shift;
my %only = map { $_ => 1 } @ARGV;
open my $f, '<', $file or die "$file: $!";
my (@lines, %start);
while (<$f>) {
    chomp;
    if (/^(_\S+|__\S+):$/) { $start{$1} = scalar @lines; push @lines, "FN $1"; next }
    push @lines, $_;
}
sub disp { my $d = shift; return $d =~ /^0x/ ? (hex($d) >= 0x80000000 ? hex($d) - 2**32 : hex $d) : $d }
for my $fn (sort keys %start) {
    next if %only && !$only{$fn};
    my $i = $start{$fn} + 1;
    my @k;
    while ($i < @lines && $lines[$i] !~ /^FN /) { push @k, $lines[$i++] }
    my @calls = grep { $k[$_] =~ /calll\s+_SendFontManagementMessage/ } 0 .. $#k;
    next unless @calls;
    for my $r (@calls) {
        my ($type, $buffer) = ('?', undef);
        for (my $j = $r - 1; $j >= 0 && $j >= $r - 30; $j--) {
            $type = sprintf "0x%x", hex $1 if $type eq '?' && $k[$j] =~ /movl\s+\$0x([0-9a-f]+),\(%esp\)$/;
            if (!defined $buffer && $k[$j] =~ /movl\s+%(\w+),0x04\(%esp\)$/) {
                my $reg = $1;
                for (my $m = $j - 1; $m >= 0 && $m >= $j - 40; $m--) {
                    if ($k[$m] =~ /leal\s+(-?0x[0-9a-f]+)\(%ebp\),%$reg$/) { $buffer = disp($1); last }
                    if ($k[$m] =~ /movl\s+(-?0x[0-9a-f]+)\(%ebp\),%$reg$/) {   # through a stack slot
                        my $slot = $1;
                        for (my $q = $m - 1; $q >= 0; $q--) {
                            if ($k[$q] =~ /movl\s+%(\w+),\Q$slot\E\(%ebp\)$/) {
                                my $r2 = $1;
                                for (my $s = $q - 1; $s >= 0 && $s >= $q - 20; $s--) {
                                    if ($k[$s] =~ /leal\s+(-?0x[0-9a-f]+)\(%ebp\),%$r2$/) { $buffer = disp($1); last }
                                }
                                last;
                            }
                        }
                        last;
                    }
                }
            }
        }
        unless (defined $buffer) { print "$fn: type $type, buffer unknown\n"; next }
        my (%request, %reply);
        for my $j (0 .. $#k) {
            my $l = $k[$j];
            my $width = $l =~ /\s(movl|cmpl|addl|subl|andl|orl|testl|incl|decl|pushl)\s/ ? 4
                      : $l =~ /\s(movw|movzwl|movswl|cmpw|testw)\s/ ? 2
                      : $l =~ /\s(movb|movzbl|movsbl|cmpb|testb)\s/ ? 1 : $l =~ /\sleal\s/ ? 'addr' : 0;
            next unless $width;
            while ($l =~ /(-?0x[0-9a-f]+)\(%ebp\)/g) {
                my $o = disp($1) - $buffer;
                next if $o < 0 || $o >= 0x6cc;
                my $dest = $j < $r ? \%request : \%reply;
                $dest->{$o}{$width}++;
            }
        }
        printf "%s: type %s, buffer %d\n", $fn, $type, $buffer;
        for my $part (['request', \%request], ['reply', \%reply]) {
            my ($name, $h) = @$part;
            next unless %$h;
            print "  $name:";
            for my $o (sort { $a <=> $b } keys %$h) { printf " %x:%s", $o, join '/', sort keys %{$h->{$o}} }
            print "\n";
        }
    }
}
