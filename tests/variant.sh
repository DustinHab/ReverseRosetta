#!/bin/sh
# variant.sh NAME "EXTRA_FLAGS" - build the translator with other compiler options as build/NAME (A/B with ab.sh BIN=NAME)
R=$(cd "$(dirname "$0")/.." && pwd)
cd $R || exit 1
N=$1; X=$2
gcc-4.2 -arch ppc -mcpu=7450 -Wall -Wno-unused-function -std=gnu99 ${X:--O3 -mdynamic-no-pic} -o build/$N main.c macho.c decoder.c interp.c fpu.c \
    sys.c mach_msg.c commpage.c sse.c thunk.c jit.c thread.c -Wl,-pagezero_size,0x10000000 && ls -l build/$N
