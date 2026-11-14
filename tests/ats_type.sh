#!/bin/sh
# ats_type.sh TYPE [INTEL_FUNCTION] [LINES] - the first requests/replies of this type from the last run and the
# sending function in the Intel ATS
R=$(cd "$(dirname "$0")/.." && pwd)
awk -v t="$1" '/^\[ats to\]/ { z = ($4 == t) } z && /^\[ats/ { print substr($0, 1, 700); if (++k > 5) exit }' /tmp/app_out.txt
[ -n "$2" ] && sh $R/tests/ats_function.sh $2 ${3:-100}
