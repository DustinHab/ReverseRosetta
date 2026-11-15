#!/bin/sh
# cli_matrix.sh [list|work|all] - the universal command line programs of the system (their i386 part) under rosetta
# against the native run: same output and exit status?
#   list  every program in /bin and /usr/bin (and a few from /sbin, /usr/sbin) without arguments, input /dev/null
#   work  programs doing real work (text, archives, scripting languages, plists, databases, crypto)
#   all   both (default)
# Each command runs natively, translated, and natively again (output that changes between the native runs is
# "unstable"), in an empty working directory, with a wall-clock limit. Results: $OUT/results.txt (one line per command),
# $OUT/diff/ (differences), $OUT/rosetta.txt (messages of the translator, e.g. calls that are not implemented).
# Programs that could send, print, schedule, change settings or start services are never run (SKIP below).
R=$(cd "$(dirname "$0")/.." && pwd)
ROS=$R/build/rosetta
T=$R/tests
OUT=${OUT:-/tmp/rr_matrix}
TO=${TIMEOUT:-20}
MODE=${1:-all}
rm -rf "$OUT"; mkdir -p "$OUT/diff" "$OUT/cwd"
: > "$OUT/results.txt"; : > "$OUT/rosetta.txt"

# never run: mail, print, at/cron/uucp queues, clipboard, network, services, accounts and passwords, disks, network
# and power settings, prebinding of system files, programs with windows, terminal editors and screen programs,
# endless output, CPAN and perlbug (configuration dialog, mail)
SKIP=" reboot halt shutdown fasthalt fastboot init launchd crontab at atq atrm batch leave lpr lp lpq lprm lpstat
 lpadmin lpoptions lppasswd cancel cupsenable cupsdisable cupsaccept cupsreject accept reject mail mailx Mail
 sendmail newaliases postfix postqueue postsuper postdrop postalias postcat postconf postkick postlock postlog
 postmap postmulti smtpd procmail fetchmail formail uucp uux uuto uupick uustat uuname uulog cu logger syslog
 pbcopy pbpaste ssh-agent ssh-keygen ssh-add ssh-keyscan ssh scp sftp slogin telnet ftp tftp rlogin rsh rcp rexec
 talk write wall mesg biff screen script yes top login su sudo newgrp passwd chkpasswd chsh chfn chpass vipw visudo
 dsenableroot dsimport createhomedir dscl dseditgroup dsconfigad dsconfigldap softwareupdate installer
 system_profiler ioreg say afplay open open-x11 osascript osacompile automator screencapture hdiutil hdid diskutil
 drutil bless nvram pmset systemsetup networksetup scselect scutil ipconfig ifconfig arp ndp route ipfw killall kill
 pkill xcodebuild opendiff xed ibtool emacs emacsclient vi vim view ex rview rvim vimtutor nano pico ntpd sshd httpd
 apachectl named rndc kinit kdestroy kpasswd ktutil kadmin kswitch automount mount umount mdutil tmutil
 update_prebinding redo_prebinding dig host nslookup whois finger ab nc ping ping6 traceroute traceroute6 curl wget
 rsync smbclient smbpasswd net wish wish8.4 idle idle2.5 appletviewer jconsole policytool javaws cpan instmodsh
 perlbug perlthanks atsutil spindump stackshot pcastconfig podcast xgrid xgridstatus agentxtool last "

# real work: label|command (run with sh -c in the working directory; the program is the first word)
work_list() {
cat <<'EOF'
cat|/bin/cat /etc/services
wc|/usr/bin/wc -lwc /usr/share/dict/words
sort|/usr/bin/sort -t: -k3 -n /etc/passwd
sort big|/usr/bin/sort -r /usr/share/dict/words
uniq|/usr/bin/uniq -c /etc/services
grep|/usr/bin/grep -c -i -E 'tcp|udp' /etc/services
grep regex|/usr/bin/grep -E '^(ab|qu)[a-z]{3,}ing$' /usr/share/dict/words
awk|/usr/bin/awk -F: '{ n += NF; s = s $1 } END { print n, length(s) }' /etc/passwd
awk math|/usr/bin/awk 'BEGIN { for (i = 1; i < 200000; i++) s += sin(i) * log(i); printf "%.6f\n", s }'
sed|/usr/bin/sed -e 's/[aeiou]/_/g' -e '/^#/d' /etc/services
tr|/usr/bin/tr 'a-z' 'A-Z' < /usr/share/dict/propernames
cut|/usr/bin/cut -d: -f1,3 /etc/passwd
paste|/usr/bin/paste -d, /etc/shells /etc/shells
join|/usr/bin/join /usr/share/dict/connectives /usr/share/dict/connectives
head tail|/usr/bin/head -2000 /usr/share/dict/words | /usr/bin/tail -5
split|/usr/bin/split -l 50000 /usr/share/dict/words part. && /bin/ls part.* && /bin/cat part.* | /usr/bin/cksum
cksum|/usr/bin/cksum /usr/share/dict/words
md5|/sbin/md5 /usr/share/dict/words
shasum|/usr/bin/shasum -a 256 /etc/services
openssl sha1|/usr/bin/openssl sha1 /usr/share/dict/words
openssl enc|/usr/bin/openssl enc -aes-128-cbc -K 000102030405060708090a0b0c0d0e0f -iv 0f0e0d0c0b0a09080706050403020100 -in /etc/services | /usr/bin/openssl md5
openssl rsa|/usr/bin/openssl genrsa -rand /etc/services 512 2>/dev/null | /usr/bin/head -1
openssl speed|/usr/bin/openssl speed -elapsed md5 2>&1 | /usr/bin/grep -c md5
gzip|/usr/bin/gzip -9 -c /usr/share/dict/words | /usr/bin/gzip -d -c | /usr/bin/cksum
gzip ratio|/usr/bin/gzip -6 -c /usr/share/dict/words | /usr/bin/wc -c
bzip2|/usr/bin/bzip2 -c /usr/share/dict/words | /usr/bin/bzip2 -d -c | /usr/bin/cksum
bzip2 size|/usr/bin/bzip2 -9 -c /usr/share/dict/words | /usr/bin/wc -c
zip|/usr/bin/zip -q -r t.zip /usr/share/man/man5 && /usr/bin/unzip -l t.zip | /usr/bin/tail -1
tar|/usr/bin/tar cf - -C /usr/share/man man5 | /usr/bin/tar tvf - | /usr/bin/wc -l
tar gz|/usr/bin/tar czf t.tgz -C /usr/share man/man5 && /usr/bin/tar xzf t.tgz && /usr/bin/find man -type f | /usr/bin/wc -l
pax|/bin/pax -w -f t.pax /etc/hosts /etc/shells && /bin/pax -f t.pax
cpio|/usr/bin/find /usr/share/man/man5 -name '*.5' | /usr/bin/cpio -o 2>/dev/null | /usr/bin/wc -c
find|/usr/bin/find /usr/share/man -name '*.1' -size +8 | /usr/bin/wc -l
xargs|/usr/bin/find /usr/share/man/man5 -name '*.5' -print0 | /usr/bin/xargs -0 /usr/bin/wc -c | /usr/bin/tail -1
ls|/bin/ls -lan /usr/lib
ls R|/bin/ls -R /usr/share/zoneinfo
stat|/usr/bin/stat -f '%N %Sp %z' /etc/hosts /etc/services
du|/usr/bin/du -sk /usr/share/man/man5
df|/bin/df -k / | /usr/bin/awk '{ print $1 }'
diff|/usr/bin/diff /etc/services /etc/protocols | /usr/bin/wc -l
cmp|/usr/bin/cmp /etc/services /etc/protocols; echo $?
comm|/usr/bin/comm -12 /usr/share/dict/connectives /usr/share/dict/connectives | /usr/bin/wc -l
od|/usr/bin/od -A x -t x1z /etc/hosts | /usr/bin/head -20
hexdump|/usr/bin/hexdump -C /bin/ls | /usr/bin/head -30
strings|/usr/bin/strings -a /bin/ls | /usr/bin/head -40
file|/usr/bin/file /bin/ls /etc/hosts /usr/share/dict/words /usr/lib/libz.dylib
lipo|/usr/bin/lipo -info /bin/ls /usr/lib/libSystem.B.dylib
otool|/usr/bin/otool -L /bin/ls
otool tv|/usr/bin/otool -arch i386 -tv /bin/echo | /usr/bin/head -60
nm|/usr/bin/nm -arch i386 /usr/lib/libz.dylib | /usr/bin/head -50
expr|/bin/expr 12345 \* 6789 + 1
bc|echo 'scale=60; 4*a(1)' | /usr/bin/bc -l
dc|echo '2 200 ^ p' | /usr/bin/dc
jot|/usr/bin/jot -r -s ' ' 5 1 100 1 | /usr/bin/wc -w; /usr/bin/jot 20 1 | /usr/bin/paste -s -d+ - | /usr/bin/bc
printf|/usr/bin/printf '%5.2f %x %o %s %e\n' 3.14159 255 8 text 123456.789
date|/bin/date -u -r 1000000000 '+%Y-%m-%d %H:%M:%S %A %j'
cal|/usr/bin/cal 9 1752
iconv|/usr/bin/iconv -f utf-8 -t utf-16 /etc/services | /usr/bin/wc -c
vis|/usr/bin/vis -c /bin/echo | /usr/bin/head -c 300 | /usr/bin/wc -c
uuencode|/usr/bin/uuencode /etc/hosts hosts | /usr/bin/uudecode -p | /usr/bin/cksum
fold fmt|/usr/bin/fold -w 30 /etc/services | /usr/bin/fmt -w 50 | /usr/bin/wc -l
column|/usr/bin/column -t /etc/services | /usr/bin/tail -3
rev|/usr/bin/rev /etc/shells
perl|/usr/bin/perl -e 'my %h; $h{$_ % 97} += $_ for 1..200000; print join(",", map { $h{$_} } sort { $a <=> $b } keys %h), "\n"'
perl regex|/usr/bin/perl -ne 'print if /^[a-z]+(ing|tion)$/' /usr/share/dict/words | /usr/bin/wc -l
perl modules|/usr/bin/perl -MPOSIX -MData::Dumper -MStorable -MDigest::MD5=md5_hex -e 'print md5_hex("abc"), " ", floor(2.7), "\n"; $Data::Dumper::Sortkeys = 1; print Dumper({a => [1, 2], b => "x"})'
perl sprintf|/usr/bin/perl -e 'printf("%.15g %.3e %d\n", atan2(1, 1) * 4, exp(10), 2**31 - 1)'
python|/usr/bin/python -c 'print sum(x * x for x in range(300000)), len(str(2 ** 10000))'
python modules|/usr/bin/python -c 'import zlib, md5, struct, re, xml.dom.minidom, sqlite3; d = xml.dom.minidom.parseString("<a><b>x</b></a>"); print zlib.crc32("abc"), md5.new("abc").hexdigest(), struct.pack("<I", 1).encode("hex"), d.documentElement.firstChild.tagName'
python sqlite|/usr/bin/python -c 'import sqlite3; c = sqlite3.connect(":memory:"); c.execute("create table t(a, b)"); c.executemany("insert into t values (?, ?)", [(i, str(i)) for i in range(5000)]); print c.execute("select count(*), sum(a), max(b) from t").fetchone()'
python float|/usr/bin/python -c 'import math; print repr(math.pi), math.sqrt(2), math.exp(1), 1e300 * 1e10, float("nan") != float("nan")'
ruby|/usr/bin/ruby -e 'h = Hash.new(0); 200000.times { |i| h[i % 101] += i }; puts h.values.inject(0) { |s, v| s + v }'
ruby float|/usr/bin/ruby -e 'printf("%.12f %s\n", Math::PI, (1..30).inject(1) { |p, i| p * i })'
tclsh|echo 'puts [expr {sqrt(2.0) * 1e5}]; set s 0; for {set i 0} {$i < 100000} {incr i} { incr s $i }; puts $s' | /usr/bin/tclsh
sqlite3|/usr/bin/sqlite3 :memory: 'create table t(a); insert into t values (1); insert into t select a + 1 from t; insert into t select a + 2 from t; select count(*), sum(a), group_concat(a) from t;'
plutil|/usr/bin/plutil -convert xml1 -o - /System/Library/CoreServices/SystemVersion.plist
plutil lint|/usr/bin/plutil -lint /System/Library/CoreServices/SystemVersion.plist
defaults read|/usr/bin/defaults read /System/Library/CoreServices/SystemVersion ProductVersion
xmllint|/usr/bin/xmllint --format /System/Library/CoreServices/SystemVersion.plist
xsltproc|/usr/bin/xsltproc --version | /usr/bin/head -1
sw_vers|/usr/bin/sw_vers
uname|/usr/bin/uname -a
hostinfo|/usr/bin/hostinfo | /usr/bin/head -3
sysctl|/usr/sbin/sysctl kern.ostype kern.osrelease hw.ncpu hw.byteorder
id|/usr/bin/id
groups|/usr/bin/groups
whoami|/usr/bin/whoami
locale|/usr/bin/locale
env|/usr/bin/env -i A=1 B=2 /usr/bin/env
nice|/usr/bin/nice -n 5 /bin/echo nice works
time|/usr/bin/time /bin/echo timed 2>/dev/null
ps|/bin/ps -o comm -p $$
textutil|/usr/bin/textutil -convert html -stdout /etc/hosts | /usr/bin/grep -c '<p'
sips|/usr/bin/sips -g pixelWidth -g pixelHeight /System/Library/CoreServices/Finder.app/Contents/Resources/Finder.icns
mdls|/usr/bin/mdls -name kMDItemContentType /etc/hosts
xattr|/usr/bin/touch x && /usr/bin/xattr -w test.attr value x && /usr/bin/xattr -p test.attr x
mkfile|/usr/sbin/mkfile 64k f && /bin/ls -l f | /usr/bin/awk '{ print $5 }'
dd|/bin/dd if=/usr/share/dict/words bs=4k count=100 2>/dev/null | /usr/bin/cksum
ln cp mv|/bin/cp /etc/hosts a && /bin/ln -s a b && /bin/mv b c && /bin/cat c | /usr/bin/wc -l && /bin/rm a c
chmod|/usr/bin/touch m && /bin/chmod 640 m && /usr/bin/stat -f '%Sp' m
tput|/usr/bin/tput -T vt100 cols
units|/usr/bin/units '3 feet' 'meters'
look|/usr/bin/look quart
spell words|/usr/bin/grep -c '^q' /usr/share/dict/words
iostat|/usr/sbin/iostat -c 1 -w 1 > /dev/null; echo $?
vm_stat|/usr/bin/vm_stat | /usr/bin/head -1
zdump|/usr/sbin/zdump -v Europe/Berlin | /usr/bin/head -5
lsbom|/usr/bin/lsbom -s /Library/Receipts/BSD.pkg/Contents/Archive.bom 2>/dev/null | /usr/bin/head -5
man|/usr/bin/man -w ls
apropos|/usr/bin/apropos -s 1 checksum 2>/dev/null | /usr/bin/head -3
EOF
}

native() {     # run k: native command line in a fresh working directory
    rm -rf "$OUT/cwd"; mkdir -p "$OUT/cwd"
    (cd "$OUT/cwd" && sh "$T/with_timeout.sh" "$TO" sh -c "$cmd" < /dev/null > "$OUT/n$1.txt" 2>&1); echo $? > "$OUT/n$1.code"
}

classify() {    # label, command line (sh -c)
    label=$1; cmd=$2
    tag=$(echo "$label" | tr -c 'A-Za-z0-9\n' '_')
    native 1
    rm -rf "$OUT/cwd"; mkdir -p "$OUT/cwd"
    # translated: every program with an i386 part in the command line runs through rosetta (absolute paths;
    # scripts stay as they are). Decided here, so no wrapper process changes the environment.
    rcmd=$(printf '%s' "$cmd" | ROS=$ROS perl -pe 's{(^|[|;&(]\s*)(/[^\s|;&()<>]+)}{ my ($p, $f) = ($1, $2);
        my $i = -f $f ? `lipo -info $f 2>/dev/null` : ""; $p . ($i =~ /i386/ ? "$ENV{ROS} $f" : $f) }ge')
    start=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')
    (cd "$OUT/cwd" && sh "$T/with_timeout.sh" "$TO" sh -c "$rcmd" < /dev/null > "$OUT/r.txt" 2>&1); echo $? > "$OUT/r.code"
    took=$(perl -MTime::HiRes=time -e "printf '%.0f', (time - $start) * 1000")
    native 2                                 # after the translated run: output that depends on the time differs
    grep '^rosetta:' "$OUT/r.txt" | sed "s#^#$label: #" >> "$OUT/rosetta.txt"
    perl -ne 'print unless /^rosetta:/' "$OUT/r.txt" > "$OUT/r2.txt"     # keeps a missing final newline
    if cmp -s "$OUT/r2.txt" "$OUT/n1.txt" && cmp -s "$OUT/r.code" "$OUT/n1.code"; then res=same
    elif ! cmp -s "$OUT/n1.txt" "$OUT/n2.txt" || ! cmp -s "$OUT/n1.code" "$OUT/n2.code"; then res=unstable
    elif [ "$(cat "$OUT/r.code")" = 124 ]; then res=TIMEOUT
    elif [ "$(cat "$OUT/r.code")" -ge 128 ]; then res=CRASH
    else res=DIFF
    fi
    case $res in same|unstable) ;; *)
        { echo "== $label: $cmd"; echo "-- exit native $(cat "$OUT/n1.code"), translated $(cat "$OUT/r.code"); < native, > translated"
          diff "$OUT/n1.txt" "$OUT/r2.txt" | head -40; } > "$OUT/diff/$tag.txt" ;;
    esac
    printf '%-9s %6s ms  %s\n' "$res" "$took" "$label" >> "$OUT/results.txt"
}

if [ "$MODE" = list ] || [ "$MODE" = all ]; then
    for f in /bin/* /usr/bin/* /sbin/md5 /sbin/ping /usr/sbin/sysctl /usr/sbin/lsof /usr/sbin/kextstat /usr/sbin/zdump \
             /usr/sbin/lsbom /usr/sbin/netstat /usr/sbin/iostat /usr/sbin/mkfile /usr/sbin/chown; do
        [ -f "$f" ] && [ -x "$f" ] || continue
        b=$(basename "$f")
        case "$SKIP" in *" $b "*) continue ;; esac
        lipo -info "$f" 2>/dev/null | grep -q 'i386' || continue
        classify "$b" "$f"
    done
fi
if [ "$MODE" = work ] || [ "$MODE" = all ]; then
    work_list | while IFS='|' read -r label cmd; do classify "work: $label" "$cmd"; done
fi
rm -rf "$OUT/cwd"
echo "$(grep -c '^same' "$OUT/results.txt") same, $(grep -c '^unstable' "$OUT/results.txt") unstable natively," \
     "$(grep -c '^DIFF' "$OUT/results.txt") different, $(grep -c '^CRASH' "$OUT/results.txt") crashed," \
     "$(grep -c '^TIMEOUT' "$OUT/results.txt") timed out"
grep -v '^same' "$OUT/results.txt" | grep -v '^unstable'
echo "translator messages:"; sed 's/(eip.*//' "$OUT/rosetta.txt" | sort | uniq -c | sort -rn | head -30
