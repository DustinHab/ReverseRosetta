#!/bin/sh
# pkg_signature.sh PACKAGE - show the signature of a flat installer package (xar): certificates in the table of contents
P=${1:-/tmp/combo_mount/MacOSXUpdCombo10.5.8.pkg}
xar --dump-toc=/tmp/pkg_toc.xml -f "$P" || exit 1
echo "files in package: $(grep -c '<file ' /tmp/pkg_toc.xml)"
grep -o '<signature style="[A-Za-z0-9]*"' /tmp/pkg_toc.xml | head -2
# decode each certificate (Base64 over several lines) separately
awk '/<X509Certificate>/ { z = 1; s = "" } z { s = s $0 } /<\/X509Certificate>/ { z = 0; gsub(/.*<X509Certificate>|<\/X509Certificate>.*|[ \t]/, "", s); print s }' /tmp/pkg_toc.xml > /tmp/pkg_cert.txt
n=0
while read -r z; do
  n=$((n + 1))
  echo "$z" | openssl base64 -d -A > /tmp/pkg_cert_$n.der
  echo "certificate $n:"
  openssl x509 -inform DER -in /tmp/pkg_cert_$n.der -noout -subject -issuer
done < /tmp/pkg_cert.txt
