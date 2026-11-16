#!/bin/sh
# sdk_stub.sh INSTALL_PATH... - a stub library with every symbol that the SDK's i386 stub of this library exports
# (the 10.5 SDK has them for all public frameworks), for subframeworks that the Combo Update does not contain as i386
# (DictionaryServices, Help, ImageKit ...): umbrellas can then re-export them (intel_umbrellas.sh) and programs that
# take a symbol from the umbrella load. Functions report "missing" on their first call and return an error code
# (Get/Is/Has/Release...: 0; EnterMovies and ExitMovies succeed: programs that play only an intro film go on), data symbols become CFStrings with their name (notification names and keys), Objective-C
# class symbols are absolute 0, as in intel_stubs.sh. An existing library that is not a stub library stays; an existing
# stub library keeps the symbols it had (private ones that programs imported) besides those of the SDK.
R=$(cd "$(dirname "$0")/.." && pwd)
W=${ROSETTA_INTEL:-$R/intel/root}
SDK=/Developer/SDKs/MacOSX10.5.sdk
for m in "$@"; do
    S=$SDK$m
    if [ -f "$W$m" ] && ! grep -q -e "rosetta-stub" -e "rosetta-Platzhalter" "$W$m"; then echo "$m: exists, kept"; continue; fi
    # not in the SDK (private frameworks such as GraphKit): an existing stub library is rebuilt from its own symbols
    if [ ! -f "$S" ]; then [ -f "$W$m" ] || { echo "$m: not in the SDK"; continue; }; S=$W$m; fi
    n=$(basename "$m" | sed 's/\.dylib$//; s/\..*//')
    ver=$(otool -arch i386 -L "$S" | sed -n 2p)
    cv=$(echo "$ver" | sed 's/.*compatibility version \([0-9.]*\).*/\1/')
    cur=$(echo "$ver" | sed 's/.*current version \([0-9.]*\).*/\1/')
    nm -arch i386 -g "$S" 2>/dev/null | awk '
        $2 == "T" && $3 ~ /^_[A-Za-z0-9_]+$/ { print "lazy", $3 }
        ($2 == "D" || $2 == "S") && $3 ~ /^_[A-Za-z0-9_]+$/ { print "nonlazy", $3 }
        ($2 == "A" || $2 == "S") && $3 ~ /^\.objc_class_name_/ { print "class", $3 }' > /tmp/ss_sym0.txt
    if [ -f "$W$m" ]; then              # symbols of the existing stub library
        nm -arch i386 -g "$W$m" 2>/dev/null | awk '
            $2 == "T" && $3 ~ /^_[A-Za-z0-9_]+$/ { print "lazy", $3 }
            ($2 == "D" || $2 == "S") && $3 ~ /^_[A-Za-z0-9_]+$/ { print "nonlazy", $3 }
            ($2 == "A" || $2 == "S") && $3 ~ /^\.objc_class_name_/ { print "class", $3 }' >> /tmp/ss_sym0.txt
    fi
    # one kind per symbol. What a symbol is (function, data, class) the library itself knows: the SDK's stub, or else the
    # PowerPC library of the system at the same path. A program's import tells only how it is bound (a function taken
    # through a non-lazy pointer looked like data: Cinebench jumped into a CFString for QTGetPixMapPtrRowBytes)
    : > /tmp/ss_truth.txt
    if [ -f "$SDK$m" ]; then nm -arch i386 -g "$SDK$m" 2>/dev/null > /tmp/ss_truth0.txt
    elif [ -f "$m" ]; then nm -arch ppc -g "$m" > /tmp/ss_truth0.txt 2>/dev/null || nm -arch ppc7400 -g "$m" > /tmp/ss_truth0.txt 2>/dev/null
    else : > /tmp/ss_truth0.txt; fi
    awk '$2 == "T" && $3 ~ /^_/ { print "lazy", $3 } ($2 == "D" || $2 == "S") && $3 ~ /^_/ { print "nonlazy", $3 }' /tmp/ss_truth0.txt > /tmp/ss_truth.txt
    sort -u /tmp/ss_sym0.txt | awk 'FILENAME == "/tmp/ss_truth.txt" { t[$2] = $1; next }
        { k[$2] = (k[$2] == "nonlazy" || $1 == "nonlazy") ? "nonlazy" : (k[$2] == "class" || $1 == "class") ? "class" : $1 }
        END { for (s in k) print ((s in t) ? t[s] : k[s]), s }' /tmp/ss_truth.txt - | sort > /tmp/ss_sym.txt
    case "$m" in                         # error code of the framework (as intel_stubs.sh)
        *CoreVideo*) E=-6660 ;;
        *SpeechRecognition*) E=-5100 ;;
        *CarbonSound*) E=-201 ;;
        *) E=-1 ;;
    esac
    {
        echo "/* stub library for $m (sdk_stub.sh: every symbol of the SDK's stub) */"
        echo '#include <stdio.h>'
        echo '#include <CoreFoundation/CoreFoundation.h>'
        printf '%s\n' 'static void report_missing(const char *n) { fprintf(stderr, "rosetta-stub: %s missing\n", n); }'
        echo '__attribute__((used)) static const char marker[] = "rosetta-stub";'
        while read kind sym; do
            c=${sym#_}
            case "$kind" in
            class) ;;                       # a class of its own (stub_classes.sh)
            nonlazy) echo "const CFStringRef $c = CFSTR(\"$c\");" ;;
            *)  case "$c" in *Get*|*Is*|*Has*|*Release*|*Visible*|*Busy*|*Notification*|EnterMovies|ExitMovies) r=0 ;; *) r=$E ;; esac
                echo "long $c(void) { static int m; if (!m++) report_missing(\"$c\"); return $r; }" ;;
            esac
        done < /tmp/ss_sym.txt
    } > "/tmp/ss_$n.c"
    sh "$R/tools/stub_classes.sh" "$m" /tmp/ss_sym.txt > /tmp/ss_classes.m
    cls=; fw="-framework CoreFoundation"            # classes in a file of their own (no clash with Cocoa's declarations)
    [ -s /tmp/ss_classes.m ] && { cls=/tmp/ss_classes.m; fw="-framework Cocoa -framework QuartzCore"; }
    mkdir -p "$(dirname "$W$m")"
    ok=0
    for try in 1 2 3 4 5 6; do
        gcc-4.2 -arch i386 -O1 -dynamiclib -isysroot $SDK -install_name "$m" -compatibility_version "$cv" -current_version "$cur" \
            -o "$W$m" "/tmp/ss_$n.c" $cls $fw 2> /tmp/ss_err.txt && { ok=1; break; }
        # a superclass without a public declaration (NSNibConnector): the class becomes a subclass of NSObject
        x=$(sed -n "s/.*cannot find interface declaration for '\([A-Za-z0-9_]*\)'.*/\1/p" /tmp/ss_err.txt | head -1)
        [ -n "$x" ] && [ -n "$cls" ] || break
        sed -i '' "s/ : $x @end/ : NSObject @end/" $cls
    done
    if [ $ok = 1 ]; then
        case "$m" in                        # framework: Versions/Current and the top-level link
            */Versions/A/*) fw=${m%/Versions/A/*}; ( cd "$W$fw" && ln -sfh A Versions/Current && ln -sfh "Versions/Current/$n" "$n" ) ;;
        esac
        echo "$m: $(grep -c . /tmp/ss_sym.txt) symbols, version $cv/$cur"
    else
        echo "$m: build failed ($(head -2 /tmp/ss_err.txt | tr '\n' ' '))"
    fi
done
