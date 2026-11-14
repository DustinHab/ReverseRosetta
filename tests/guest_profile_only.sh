#!/bin/sh
# guest_profile_only.sh ["PROG ARGS"] [RUNS] - like guest_profile.sh, but only the guest functions (without host functions)
R=$(cd "$(dirname "$0")/.." && pwd)
Z=200 sh $R/tests/guest_profile.sh "$@" 2>&1 | grep -v 'Host: _' | head -${NLINES:-30}
