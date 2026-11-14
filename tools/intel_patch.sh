#!/bin/sh
# intel_patch.sh - targeted changes to Intel libraries in the root (not to the system). Each site is found by
# its address (after intel_rebase.sh), the original bytes are checked, and a backup is made first.
# Run again after every intel_rebase.sh / intel_prebind.sh.
#
# 1. CoreGraphics 409.3 (10.5.8) talks to the system's WindowServer (CoreGraphics 409.8): connectAndCheck rejects
#    differing versions (1008 -> exit(1) in CGSServerPort) unless CGWSAllowVersionMismatch is set (which only
#    counts together with the global default CGAllowDebuggingDefaults). The jump "mismatch allowed" at
#    0x4147f7c3 (jne +0x20) becomes unconditional (jmp +0x20).
#
# 2. Security (Intel) identifies itself to securityd (PowerPC) with ClientSetupInfo {byte order 0x1234, version
#    0x4e20}. securityd detects the swapped byte order (0x34120000: byte-swapping mode, built for Apple's Rosetta), but
#    then checks the version raw against 0x4e20 and otherwise throws CSSMERR_CSSM_INCOMPATIBLE_VERSION (SessionGetInfo,
#    LaunchServices, every AppKit program aborts). ClientSession::activate therefore sends the version so that it
#    arrives raw as 0x4e20 (movl $0x204e0000,%edi at 0x466cc5f3); securityd swaps the rest itself.
#
# 3. LaunchServices (Intel) queries the IOKit entry IODeviceTree:/efi when it registers an application bundle
#    (_LSCopyMutableBundleInfoDictionary) and without it waits forever on a condition variable of its own. A
#    PowerPC Mac has Open Firmware instead of EFI: every application bundle (Calculator ...) hangs. The jump that
#    skips the wait (je at 0x43ba09c5) becomes unconditional.
#
# Formerly 4.: ATS, RendezvousWithServer (sete at 0x411cfe34 -> movb $0). No longer needed: rosetta converts the
#    messages to the ATSServer and reports the server's other byte order in the reply to the rendezvous (byte 0x40);
#    with that the client enables its run-time byte swapping itself. Reverted here.
R=$(cd "$(dirname "$0")/.." && pwd)
W=${ROSETTA_INTEL:-$R/intel/root}
B=$R/intel/backup
mkdir -p "$B"
patch_bytes() {     # file vmaddr old new (bytes in hex, same length)
    perl -e '
        my ($f, $addr, $old, $new, $b) = @ARGV; $addr = hex $addr;
        my @seg; my %s;
        open L, "otool -arch i386 -l \"$f\" |";
        while (<L>) { %s = () if /cmd LC_SEGMENT$/; $s{v} = hex $1 if /vmaddr (0x\w+)/; $s{g} = hex $1 if /vmsize (0x\w+)/;
                      push @seg, [$s{v}, $s{g}, $1] if /fileoff (\d+)/ }
        my $off; for my $s (@seg) { $off = $addr - $s->[0] + $s->[2] if $addr >= $s->[0] && $addr < $s->[0] + $s->[1] }
        defined $off or die "address not in image\n";
        open D, "+<", $f or die "$f: $!\n"; binmode D; seek D, $off, 0; read D, my $cur, length($old) / 2;
        my $cur_h = unpack "H*", $cur;
        if ($cur_h eq $new) { print "already patched: $f\n"; exit 0 }
        $cur_h eq $old or die "unexpected bytes $cur_h instead of $old in $f\n";
        my $n = (split m{/}, $f)[-1]; system("cp", "-p", $f, "$b/$n.before_patch") == 0 or die "backup\n" unless -e "$b/$n.before_patch";
        seek D, $off, 0; print D pack("H*", $new); close D;
        print "patched: $f at ", sprintf("%08x", $addr), " ($old -> $new)\n";' "$1" "$2" "$3" "$4" "$B"
}
patch_bytes "$W/System/Library/Frameworks/ApplicationServices.framework/Versions/A/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics" \
    4147f7c3 7520 eb20
patch_bytes "$W/System/Library/Frameworks/Security.framework/Versions/A/Security" \
    466cc5f3 bf204e0000 bf00004e20
patch_bytes "$W/System/Library/Frameworks/CoreServices.framework/Versions/A/Frameworks/LaunchServices.framework/Versions/A/LaunchServices" \
    43ba09c5 7469 eb69
# revert: bytes in the opposite direction (patched -> original)
patch_bytes "$W/System/Library/Frameworks/ApplicationServices.framework/Versions/A/Frameworks/ATS.framework/Versions/A/ATS" \
    411cfe34 c6838123090000 0f948381230900
