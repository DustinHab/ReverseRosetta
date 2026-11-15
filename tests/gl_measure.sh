#!/bin/sh
# gl_measure.sh [ROUNDS] - bench_gl translated and native in the login session, side by side: milliseconds per
# frame, speed of the translated build against the native one, and whether the rendered images are the same.
# ROSETTA=path picks another build of rosetta.
R=$(cd "$(dirname "$0")/.." && pwd)
B=$R/build
N=${1:-5}
cat > /tmp/gl_measure_run.sh <<EOF
cd $B
${ROSETTA:-./rosetta} ./bench_gl $N > /tmp/gl_t.txt 2>&1
./bench_gl.ppc $N > /tmp/gl_n.txt 2>&1
EOF
TIMEOUT=${TIMEOUT:-600} sh $R/tests/in_session.sh /tmp/gl_measure_run.sh > /dev/null
head -1 /tmp/gl_n.txt
grep -v -e renderer -e 'gl error' /tmp/gl_t.txt | grep -v '^rosetta' > /tmp/gl_t2.txt
grep -v -e renderer -e 'gl error' /tmp/gl_n.txt > /tmp/gl_n2.txt
printf '%-44s %10s %10s %7s  %s\n' workload translated native speed image
paste -d '|' /tmp/gl_t2.txt /tmp/gl_n2.txt | awk -F'|' '{
  t = $1; n = $2
  name = substr(t, 1, 44); sub(/ +$/, "", name)
  split(substr(t, 45), a, " "); split(substr(n, 45), b, " ")
  same = (a[6] == b[6]) ? "same" : "DIFFERENT"
  printf "%-44s %7.3f ms %7.3f ms %6.0f %%  %s\n", name, a[1], b[1], 100 * b[1] / a[1], same
}'
grep -e 'gl error' -e '^rosetta' /tmp/gl_t.txt
