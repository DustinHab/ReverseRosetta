#!/bin/sh
# sdk_stub.sh INSTALL_PATH... - a stub library with every symbol that the SDK's i386 stub of this library exports
# (the 10.5 SDK has them for all public frameworks), for subframeworks that the Combo Update does not contain as i386
# (DictionaryServices, Help, ImageKit ...): umbrellas can then re-export them (intel_umbrellas.sh) and programs that
# take a symbol from the umbrella load. Functions report "missing" on their first call and return an error code
# (Get/Is/Has/Release...: 0), data symbols become CFStrings with their name (notification names and keys), Objective-C
# class symbols are absolute 0, as in intel_stubs.sh. An existing library that is not a stub library stays.
R=$(cd "$(dirname "$0")/.." && pwd)
W=${ROSETTA_INTEL:-$R/intel/root}
SDK=/Developer/SDKs/MacOSX10.5.sdk
for m in "$@"; do
    S=$SDK$m
    [ -f "$S" ] || { echo "$m: not in the SDK"; continue; }
    if [ -f "$W$m" ] && ! grep -q -e "rosetta-stub" -e "rosetta-Platzhalter" "$W$m"; then echo "$m: exists, kept"; continue; fi
    n=$(basename "$m" | sed 's/\.dylib$//; s/\..*//')
    ver=$(otool -arch i386 -L "$S" | sed -n 2p)
    cv=$(echo "$ver" | sed 's/.*compatibility version \([0-9.]*\).*/\1/')
    cur=$(echo "$ver" | sed 's/.*current version \([0-9.]*\).*/\1/')
    nm -arch i386 -g "$S" 2>/dev/null | awk '
        $2 == "T" && $3 ~ /^_[A-Za-z0-9_]+$/ { print "lazy", $3 }
        ($2 == "D" || $2 == "S") && $3 ~ /^_[A-Za-z0-9_]+$/ { print "nonlazy", $3 }
        $2 == "A" && $3 ~ /^\.objc_class_name_/ { print "class", $3 }' | sort -u > /tmp/ss_sym.txt
    {
        echo "/* stub library for $m (sdk_stub.sh: every symbol of the SDK's stub) */"
        echo '#include <stdio.h>'
        echo '#include <CoreFoundation/CoreFoundation.h>'
        printf '%s\n' 'static void report_missing(const char *n) { fprintf(stderr, "rosetta-stub: %s missing\n", n); }'
        echo '__attribute__((used)) static const char marker[] = "rosetta-stub";'
        while read kind sym; do
            c=${sym#_}
            case "$kind" in
            class) printf '%s\n' "asm(\".globl $sym\\n$sym = 0\");" ;;
            nonlazy) echo "const CFStringRef $c = CFSTR(\"$c\");" ;;
            *)  case "$c" in *Get*|*Is*|*Has*|*Release*|*Visible*|*Busy*|*Notification*) r=0 ;; *) r=-1 ;; esac
                echo "long $c(void) { static int m; if (!m++) report_missing(\"$c\"); return $r; }" ;;
            esac
        done < /tmp/ss_sym.txt
    } > "/tmp/ss_$n.c"
    mkdir -p "$(dirname "$W$m")"
    if gcc-4.2 -arch i386 -O1 -dynamiclib -isysroot $SDK -install_name "$m" -compatibility_version "$cv" -current_version "$cur" \
           -o "$W$m" "/tmp/ss_$n.c" -framework CoreFoundation 2> /tmp/ss_err.txt; then
        case "$m" in                        # framework: Versions/Current and the top-level link
            */Versions/A/*) fw=${m%/Versions/A/*}; ( cd "$W$fw" && ln -sfh A Versions/Current && ln -sfh "Versions/Current/$n" "$n" ) ;;
        esac
        echo "$m: $(grep -c . /tmp/ss_sym.txt) symbols, version $cv/$cur"
    else
        echo "$m: build failed ($(head -2 /tmp/ss_err.txt | tr '\n' ' '))"
    fi
done
