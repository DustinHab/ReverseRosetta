#!/bin/sh
# proof_terminal.sh X Y W H - (through in_session.sh) open a Terminal window for the demo screenshot: shows that
# Transmission is a pure Intel program and runs on this PowerPC under rosetta (proof_show.sh). Earlier
# windows of this script (recognized by their content) are closed first; other Terminal windows stay untouched.
R=$(cd "$(dirname "$0")/.." && pwd)
p=$(ps -axo pid,command | grep "rosetta $R/intel/apps/Transmission.app" | grep -v grep | awk '{ print $1 }' | head -1)
[ -n "$p" ] || { echo "Transmission is not running"; exit 1; }
osascript -e 'tell application "Terminal"' \
  -e 'repeat with w in (get windows)' \
  -e 'try' \
  -e 'set h to history of selected tab of w' \
  -e 'if h contains "lipo -info Transmission" or h contains "proof_show" then close w' \
  -e 'end try' \
  -e 'end repeat' \
  -e 'end tell'
osascript -e 'tell application "Terminal"' \
  -e "do script \"PS1='$ '; sh $R/tests/proof_show.sh $p\"" \
  -e "set size of front window to {$3, $4}" \
  -e "set position of front window to {$1, $2}" \
  -e 'activate' \
  -e 'end tell'
sleep 6
osascript -e 'tell application "Terminal" to get bounds of front window'
[ -e /private/var/db/.AccessibilityAPIEnabled ] && echo "Accessibility: on" || echo "Accessibility: off"
