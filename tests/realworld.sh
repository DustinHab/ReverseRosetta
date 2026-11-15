#!/bin/sh
# realworld.sh [ROUNDS] - ordinary programs of the system (the i386 and the PowerPC part of the same universal binary)
# on real input, translated against native: processor time (user + system) of the fastest of ROUNDS runs, for the
# translated build also the first run with an empty translation cache. Input: /usr/share/dict/words and the C headers
# of the system (/usr/include, about 14 MB as one text file). Output is discarded; translated and native output are
# compared once (cksum).
R=$(cd "$(dirname "$0")/.." && pwd)
ROS=${ROSETTA:-$R/build/rosetta}
N=${1:-3}
D=/tmp/rr_realworld
mkdir -p $D
if [ ! -s $D/text ]; then
    find /usr/include -type f -name '*.h' | sort | xargs cat > $D/text 2>/dev/null
    /usr/bin/gzip -9 -c $D/text > $D/text.gz
    /usr/bin/bzip2 -9 -c $D/text > $D/text.bz2
fi
W=/usr/share/dict/words
cat > $D/list.txt <<EOF
gzip -9 (compress)|/usr/bin/gzip -9 -c $D/text
gzip -d (decompress)|/usr/bin/gzip -d -c $D/text.gz
bzip2 -9 (compress)|/usr/bin/bzip2 -9 -c $D/text
bzip2 -d (decompress)|/usr/bin/bzip2 -d -c $D/text.bz2
sort (C headers, by line)|/usr/bin/sort $D/text
grep -c (regular expression)|/usr/bin/grep -c -E '^[a-z]+(ing|tion)$' $W
md5|/sbin/md5 $D/text
openssl sha1|/usr/bin/openssl sha1 $D/text
openssl aes-128-cbc|/usr/bin/openssl enc -aes-128-cbc -K 000102030405060708090a0b0c0d0e0f -iv 0f0e0d0c0b0a09080706050403020100 -in $D/text
perl (hash, sort)|/usr/bin/perl -e 'my %h; \$h{\$_ % 997} += \$_ for 1..600000; print join(",", map { \$h{\$_} } sort { \$a <=> \$b } keys %h), "\n"'
perl (regex over words)|/usr/bin/perl -ne 'print if /^[a-z]+(ing|tion)\$/' $W
python (loop, big integers)|/usr/bin/python -c 'print sum(x * x for x in range(600000)), len(str(3 ** 40000))'
sqlite3 (insert, query)|/usr/bin/sqlite3 :memory: 'create table t(a, b); insert into t values (1, "x"); insert into t select a + 1, b || a from t; insert into t select a + 2, b from t; insert into t select a + 4, b from t; insert into t select a + 8, b from t; insert into t select a + 16, b from t; insert into t select a + 32, b from t; insert into t select a + 64, b from t; insert into t select a + 128, b from t; insert into t select a + 256, b from t; insert into t select a + 512, b from t; insert into t select a + 1024, b from t; insert into t select a + 2048, b from t; insert into t select a + 4096, b from t; insert into t select a + 8192, b from t; select count(*), sum(a), max(length(b)) from t; select count(*) from t x, t y where x.a = y.a + 1 and y.a < 3000;'
EOF
# processor time of the children of one run of a command line (user + system), in seconds
cpu() { perl -e 'system("$ARGV[0] < /dev/null > /dev/null 2>&1"); my @t = times; printf "%.3f", $t[2] + $t[3]' "$1"; }
best() { m=""; i=0; while [ $i -lt $N ]; do t=$(cpu "$1"); m=$(perl -e 'print(($ARGV[0] eq "" || $ARGV[1] < $ARGV[0]) ? $ARGV[1] : $ARGV[0])' "$m" "$t"); i=$((i + 1)); done; echo $m; }
export ROSETTA_CACHE_FILE=$D/cache
printf '%-30s %9s %9s %9s %7s %s\n' program native first cached speed output
while IFS='|' read -r name cmd; do
    rcmd="$ROS $cmd"
    sh -c "$cmd" < /dev/null 2>&1 | cksum > $D/a.txt
    rm -f $D/cache
    first=$(cpu "$rcmd")
    sh -c "$rcmd" < /dev/null 2>&1 | grep -v '^rosetta:' | cksum > $D/b.txt
    nat=$(best "$cmd")
    tr=$(best "$rcmd")
    same=same; cmp -s $D/a.txt $D/b.txt || same=DIFFERENT
    printf '%-30s %8.2fs %8.2fs %8.2fs %6.0f%% %s\n' "$name" $nat $first $tr $(perl -e 'printf "%.1f", 100 * $ARGV[0] / ($ARGV[1] || 1)' $nat $tr) $same
done < $D/list.txt
rm -f $D/cache
