#!/bin/sh
# ats_dest_show.sh LENGTH [N] - the first N target buffers (shared memory) of this length (hex) from the last run
grep "target\] [0-9a-f]*+$1:" /tmp/app_out.txt | head -${2:-2} | cut -c1-900
