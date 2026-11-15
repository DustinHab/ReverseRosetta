#!/bin/sh
# intel_stubs.sh INSTALL_PATH... - for i386 libraries that neither the Intel root nor the system has (the Combo
# Update did not contain them: CoreVideo, SpeechRecognition, CarbonSound, SecurityHI, InstallServer ...), build stub
# libraries into the root so that dyld loads the programs that link them. Starting points: programs/frameworks whose
# dependencies are checked. Every imported function reports "missing" on its first call and returns an
# error code (Get/Is/Has/Release...: 0); symbols bound at load time (CoreVideo keys) become CFStrings with
# their name. Versions: the highest the clients require.
R=$(cd "$(dirname "$0")/.." && pwd)
D=$(cd "$(dirname "$0")" && pwd)
W=${ROSETTA_INTEL:-$R/intel/root}
S=/Developer/SDKs/MacOSX10.5.sdk
sh "$D/deps.sh" "$@" > /dev/null
[ -s /tmp/dep_missing.txt ] || { echo "nothing missing"; exit 0; }
while read m; do
    case "$m" in                            # reached through a symbolic link that deps.sh does not follow: never a stub
        /System/Library/Perl/*) echo "$m: skipped (exists under its real path)"; continue ;;
    esac
    n=$(basename "$m" | sed 's/\.dylib$//; s/\..*//')
    cv=0; cur=0
    grep "	$m\$" /tmp/dep_edges.txt | cut -f1 | sort -u > /tmp/pl_clients.txt     # (tab-separated, paths with spaces)
    while IFS= read -r k; do
        [ -f "$W$k" ] && f="$W$k" || f="$k"
        otool -arch i386 -L "$f" | grep -F "	$m (" | sed 's/.*compatibility version \([0-9.]*\), current version \([0-9.]*\).*/\1 \2/' > /tmp/pl_v.txt
        read a b < /tmp/pl_v.txt
        cv=$(printf "%s\n%s\n" "$cv" "$a" | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)
        cur=$(printf "%s\n%s\n" "$cur" "$b" | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)
    done < /tmp/pl_clients.txt
    [ "$cv" = 0 ] && cv=1.0.0
    [ "$cur" = 0 ] && cur=$cv
    case "$m" in
        *CoreVideo*) E=-6660 ;;             # kCVReturnError
        *SpeechRecognition*) E=-5100 ;;     # kSRNotAvailable
        *CarbonSound*) E=-201 ;;            # notEnoughHardwareErr
        *) E=-1 ;;
    esac
    sh "$D/imports_from.sh" "$m" > /tmp/pl_sym.txt
    if [ -f "$W$m" ] && grep -q -e "rosetta-stub" -e "rosetta-Platzhalter" "$W$m"; then
        # rebuilding a stub library: it keeps what it exported for earlier clients (so it can be rebuilt from the
        # point of view of one program, install_app.sh); the binding kind of the current clients wins
        nm -arch i386 -g "$W$m" 2>/dev/null |
            awk '$2 == "T" || $2 == "A" { print "lazy", $3 } $2 == "D" || $2 == "S" { print "nonlazy", $3 }' >> /tmp/pl_sym.txt
        awk '!seen[$2]++' /tmp/pl_sym.txt > /tmp/pl_sym2.txt && mv /tmp/pl_sym2.txt /tmp/pl_sym.txt
    fi
    {
        echo "/* stub library for $m (intel_stubs.sh) */"
        echo '#include <stdio.h>'
        echo '#include <CoreFoundation/CoreFoundation.h>'
        printf '%s\n' 'static void report_missing(const char *n) { fprintf(stderr, "rosetta-stub: %s missing\n", n); }'
        echo '__attribute__((used)) static const char marker[] = "rosetta-stub";   /* kept even without functions */'
        while read kind sym; do
            c=${sym#_}
            case "$sym" in              # class symbol of the old objc runtime: absolute, value 0 (as in the owning library)
                .objc_class_name_*) printf '%s\n' "asm(\".globl $sym\\n$sym = 0\");"; continue ;;
            esac
            if [ "$kind" = nonlazy ]; then echo "const CFStringRef $c = CFSTR(\"$c\");"
            else
                case "$c" in *Get*|*Is*|*Has*|*Release*|*Visible*|*Busy*|*Notification*) r=0 ;; *) r=$E ;; esac
                echo "long $c(void) { static int m; if (!m++) report_missing(\"$c\"); return $r; }"
            fi
        done < /tmp/pl_sym.txt
    } > "/tmp/pl_$n.c"
    mkdir -p "$(dirname "$W$m")"
    gcc-4.2 -arch i386 -O1 -dynamiclib -isysroot $S -install_name "$m" -compatibility_version "$cv" -current_version "$cur" \
        -o "$W$m" "/tmp/pl_$n.c" -framework CoreFoundation || exit 1
    case "$m" in                            # framework: Versions/Current and the top-level link
        */Versions/A/*) fw=${m%/Versions/A/*}; ( cd "$W$fw" && ln -sfh A Versions/Current && ln -sfh "Versions/Current/$n" "$n" ) ;;
    esac
    echo "$m: $(grep -c . /tmp/pl_sym.txt) symbols, version $cv/$cur, error code $E"
done < /tmp/dep_missing.txt
