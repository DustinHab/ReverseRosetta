#!/bin/sh
# cleanup.sh - kill leftover test processes (rosetta with ctrl_c/interrupt)
ps -axo pid,command | grep -E "rosetta \./(ctrl_c|interrupt)" | grep -v grep > /tmp/au.txt
cat /tmp/au.txt
while read -r p rest; do kill -9 "$p"; done < /tmp/au.txt
echo "killed: $(wc -l < /tmp/au.txt | tr -d ' ')"
