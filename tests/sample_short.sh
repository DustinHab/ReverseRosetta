#!/bin/sh
# sample_short.sh - report of sample (/tmp/app_sample.txt) shortened: the top levels of each thread with counts
awk '/^Call graph:/ { p = 1 } /^Total number in stack/ { p = 0 } p' /tmp/app_sample.txt | \
    awk '{ n = match($0, /[0-9]/); if (n > 0 && n < 40) print }' | cut -c1-150 | head -${1:-70}
