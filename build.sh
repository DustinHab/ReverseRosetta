#!/bin/sh
# Build the translator on a PowerPC Mac with Mac OS X 10.5 and Xcode 3.1 (gcc-4.2, 10.5 SDK): ./build.sh [tests]
# The host reserves the lowest 256 MB of the address space for the guest (-pagezero_size) and lives above it.
set -e
Q=$(cd "$(dirname "$0")" && pwd)
B=$Q/build
mkdir -p "$B"
CF="-arch ppc -mcpu=7450 -O3 -mdynamic-no-pic -Wall -Wno-unused-function -std=gnu99"   # no PIC: startup -6 %, -O3: another -2 %
gcc-4.2 $CF -faltivec -c -o "$B/gl.o" "$Q"/gl.c     # OpenGL: AltiVec for vertex data
gcc-4.2 $CF -o "$B/rosetta" "$Q"/main.c "$Q"/macho.c "$Q"/decoder.c "$Q"/interp.c "$Q"/fpu.c "$Q"/sys.c \
    "$Q"/mach_msg.c "$Q"/commpage.c "$Q"/sse.c "$Q"/thunk.c "$Q"/al.c "$Q"/ca.c "$Q"/gcall.c "$Q"/tec.c "$B"/gl.o "$Q"/jit.c "$Q"/thread.c -Wl,-pagezero_size,0x10000000
ls -l "$B/rosetta"
if [ "$1" = tests ] && [ -d "$Q/tests" ]; then
  S=/Developer/SDKs/MacOSX10.5.sdk
  for t in "$Q"/tests/*.c; do
    n=$(basename "$t" .c)
    case "$n" in
      raw*) gcc-4.2 -arch i386 -O1 -static -nostdlib -e _start -o "$B/$n" "$t" ;;
      cmpsb|incdec8|mem16|cond_sizes|stack_fw|narrow_ops) gcc-4.2 -arch i386 -O2 -o "$B/$n" "$t" ;;           # i386 assembly: compared against the interpreter
      faults|identity|remap) gcc-4.2 -arch i386 -O1 -o "$B/$n" "$t" ;; # i386 only: compared against tests/$n.expected
      mmx_ops) gcc-4.2 -arch i386 -Os -mmmx -msse -o "$B/$n" "$t" ;;   # MMX: against tests/mmx_ops.expected (x86; -O1 makes gcc-4.2 fail)
      pd_ops|vec_ops) gcc-4.2 -arch i386 -O2 -msse2 -o "$B/$n" "$t" ;;   # SSE2 intrinsics: compared against the interpreter
      vdsp_d) V=$S/System/Library/Frameworks/Accelerate.framework/Versions/A/Frameworks/vecLib.framework/Versions/A
              gcc-4.2 -arch i386 -O2 -isysroot $S -o "$B/$n" "$t" $V/libvDSP.dylib $V/libvMisc.dylib ;;   # Intel vecLib
      graphics*|fonts) gcc-4.2 -arch i386 -O2 -isysroot $S -o "$B/$n" "$t" -framework ApplicationServices
          gcc-4.2 -arch ppc -mcpu=7450 -O2 -o "$B/$n.ppc" "$t" -framework ApplicationServices ;;   # tests/in_session.sh
      calculate) C=System/Library/PrivateFrameworks/Calculate.framework/Versions/A/Calculate   # private engine
          [ -f "$Q/intel/root/$C" ] && gcc-4.2 -arch i386 -O1 -o "$B/$n" "$t" "$Q/intel/root/$C"   # (stub library)
          gcc-4.2 -arch ppc -O1 -o "$B/$n.ppc" "$t" "/$C" ;;
      bench_gl) gcc-4.2 -arch i386 -O2 -isysroot $S -o "$B/$n" "$t" -framework OpenGL
          gcc-4.2 -arch ppc -mcpu=7450 -O2 -o "$B/$n.ppc" "$t" -framework OpenGL ;;   # tests/gl_measure.sh
      openal) gcc-4.2 -arch i386 -O2 -isysroot $S -o "$B/$n" "$t" -framework OpenAL
          gcc-4.2 -arch ppc -mcpu=7450 -O2 -o "$B/$n.ppc" "$t" -framework OpenAL ;;   # al.c against the system's OpenAL
      textenc) gcc-4.2 -arch i386 -O1 -isysroot $S -o "$B/$n" "$t" -framework CoreServices 2> /dev/null
          gcc-4.2 -arch ppc -O1 -o "$B/$n.ppc" "$t" -framework CoreServices 2> /dev/null ;;   # tec.c against the system's TEC
      coreaudio) gcc-4.2 -arch i386 -O1 -isysroot $S -o "$B/$n" "$t" -framework AudioUnit -framework AudioToolbox -framework CoreAudio -framework CoreServices 2> /dev/null
          gcc-4.2 -arch ppc -O1 -o "$B/$n.ppc" "$t" -framework AudioUnit -framework AudioToolbox -framework CoreAudio -framework CoreServices 2> /dev/null ;;   # ca.c against the system's HAL
      quicktime|bench_qt) gcc-4.2 -arch i386 -O1 -isysroot $S -o "$B/$n" "$t" -framework QuickTime -framework ApplicationServices -framework CoreServices 2> /dev/null
          gcc-4.2 -arch ppc -O1 -o "$B/$n.ppc" "$t" -framework QuickTime -framework ApplicationServices -framework CoreServices 2> /dev/null ;;   # needs QuickTime 7.6.4 in the Intel root
      cf) gcc-4.2 -arch i386 -O2 -isysroot $S -o "$B/$n" "$t" -framework CoreFoundation
          gcc-4.2 -arch ppc -mcpu=7450 -O2 -o "$B/$n.ppc" "$t" -framework CoreFoundation ;;
      *) gcc-4.2 -arch i386 -O2 -msse2 -o "$B/$n" "$t"
         gcc-4.2 -arch ppc -mcpu=7450 -O2 -mno-fused-madd -o "$B/$n.ppc" "$t" ;;   # the same program built natively
    esac
  done
  for t in "$Q"/tests/*.m; do              # Objective-C with Foundation
    n=$(basename "$t" .m)
    f="-framework Foundation"
    [ "$n" = objc_plain ] && f=""           # only the objc runtime (no Foundation)
    [ "$n" = graphics2 ] && f="-framework Cocoa"     # a window
    gcc-4.2 -arch i386 -O2 -isysroot $S -o "$B/$n" "$t" $f -lobjc
    gcc-4.2 -arch ppc -mcpu=7450 -O2 -o "$B/$n.ppc" "$t" $f -lobjc
  done
  # native helpers for the window tests (tests/capture_rect.sh, app_windows.sh) and for host addresses
  T=$Q/tools
  gcc-4.2 -O2 -o "$T/screen_grab" "$T/screen_grab.c" -framework ApplicationServices -framework CoreServices -framework IOKit
  gcc-4.2 -O2 -o "$T/window_list" "$T/window_list.c" -framework ApplicationServices -framework Carbon
  gcc-4.2 -O2 -o "$T/host_symbol" "$T/host_symbol.c" -framework ApplicationServices -framework Carbon -framework Cocoa
  gcc-4.2 -O2 -o "$T/input_send" "$T/input_send.c" -framework ApplicationServices
  ls "$B"
fi
