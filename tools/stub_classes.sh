#!/bin/sh
# stub_classes.sh LIBRARY SYMBOLS - Objective-C source for the classes of a stub library: SYMBOLS lists the symbols
# (lines "kind symbol"); for every .objc_class_name_X an empty class X with the superclass that the PowerPC library of
# the same path gives it (tools/objc_classes.c), superclasses of the same library first. Messages they do not have
# are answered with nil and reported once (+[QTMovie movieUnfilteredPasteboardTypes] ended Keynote with an exception).
# A superclass from Foundation,
# AppKit or QuartzCore is taken as it is, any other becomes NSObject. Without class symbols nothing is printed.
# (Before, class symbols were absolute 0: a subclass in a program, of QTMovieView for instance, stayed unconnected in
# the old Objective-C runtime and the first message to it crashed: Keynote, Pages, Numbers.)
R=$(cd "$(dirname "$0")/.." && pwd)
L=$1; SYM=$2
grep -q '\.objc_class_name_' "$SYM" || exit 0
T=/tmp/objc_classes_ppc
[ -x $T ] && [ $T -nt "$R/tools/objc_classes.c" ] || gcc-4.2 -arch ppc -O1 -o $T "$R/tools/objc_classes.c" -lobjc || exit 1
: > /tmp/sc_known.txt
[ -f "$L" ] && $T "$L" > /tmp/sc_known.txt 2>/dev/null
sed -n 's/.*\.objc_class_name_\([A-Za-z0-9_]*\).*/\1/p' "$SYM" | sort -u > /tmp/sc_want.txt
cat <<'EOF'
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
/* messages the class does not have: answered with nil (0), reported once per selector, as the stub functions do */
static NSMethodSignature *rs_sig(void) { return [NSMethodSignature signatureWithObjCTypes:"@@:"]; }
static void rs_fwd(id self, NSInvocation *inv, int meta)
{
    static NSMutableSet *said;
    id r = nil;
    NSString *k = [NSString stringWithFormat:@"%c[%s %s]", meta ? 43 : 45, meta ? class_getName((Class)self) : object_getClassName(self), sel_getName([inv selector])];
    if (!said) said = [[NSMutableSet alloc] init];
    if (![said containsObject:k]) { [said addObject:k]; fprintf(stderr, "rosetta-stub: %s missing\n", [k UTF8String]); }
    [inv setReturnValue:&r];
}
#define RS_FORWARD \
    + (NSMethodSignature *)methodSignatureForSelector:(SEL)s { NSMethodSignature *m = [super methodSignatureForSelector:s]; return m ? m : rs_sig(); } \
    - (NSMethodSignature *)methodSignatureForSelector:(SEL)s { NSMethodSignature *m = [super methodSignatureForSelector:s]; return m ? m : rs_sig(); } \
    + (void)forwardInvocation:(NSInvocation *)i { rs_fwd(self, i, 1); } \
    - (void)forwardInvocation:(NSInvocation *)i { rs_fwd(self, i, 0); }
EOF
perl -e '
    my ($known, $want, $lib) = @ARGV;
    my (%sup, %img, @want, %in);
    open K, $known; while (<K>) { chomp; my ($c, $s, $i) = split /\t/; $sup{$c} = $s; $img{$c} = $i; } close K;
    open W, $want; while (<W>) { chomp; push @want, $_; $in{$_} = 1; } close W;
    my %done; my @out;
    sub superclass { my $c = shift; my $s = $sup{$c};
        return $s if defined $s && ($in{$s} || ($s ne "" && $img{$c} eq $lib));  # defined in this library
        return $s if defined $s && $s ne "" && $img{$c} =~ m{/(Foundation|AppKit|CoreFoundation|QuartzCore)\.framework/|/libobjc};
        return "NSObject"; }
    sub emit { my $c = shift; return if $done{$c}++; my $s = superclass($c); emit($s) if $in{$s} || ($s ne "NSObject" && $img{$c} eq $lib);
        push @out, "\@interface $c : $s \@end\n\@implementation $c RS_FORWARD \@end"; }
    emit($_) for @want;
    print join("\n", @out), "\n";
' /tmp/sc_known.txt /tmp/sc_want.txt "$L"
