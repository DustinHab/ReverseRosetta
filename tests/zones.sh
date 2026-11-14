#!/bin/sh
# zones.sh - usage of the fixed kernel zone "kernel map entries" (overflowed on 14 Nov and caused a panic)
# and of the related zones; column "cur #elts" against "max #elts"
zprint | awk 'NR <= 3 || /kernel.map.entries|VM.map.entries|kalloc.16384|ipc.ports|threads|tasks|kernel.stacks/'
