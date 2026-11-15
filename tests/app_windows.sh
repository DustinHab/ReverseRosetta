#!/bin/sh
# app_windows.sh NAME - windows of the running Intel program NAME (under rosetta): process, window positions, log
R=$(cd "$(dirname "$0")/.." && pwd)
p=$(ps -axo pid,command | grep "rosetta $R/intel/apps/$1.app" | grep -v grep | awk '{ print $1 }' | head -1)
[ -n "$p" ] || { echo "$1 is not running"; exit 1; }
ps -o pid,stat,time,rss -p $p
$R/tools/window_list $p
echo "--- log"
tail -${2:-10} "/tmp/app_$(echo "$1" | tr ' ' '_').log"
