#!/bin/sh
# proof_show.sh PID - (runs in the Terminal window of the demo screenshot) show the Transmission version, the architecture
# of the file, the hardware of the machine and the running process; each output with its command before it
R=$(cd "$(dirname "$0")/.." && pwd)
p=$1
cd $R/intel/apps/Transmission.app/Contents || exit 1
show() { printf '$ %s\n' "$1"; eval "$1"; }
clear
show 'defaults read "$PWD/Info" CFBundleShortVersionString'
show 'file MacOS/Transmission'
show 'system_profiler SPHardwareDataType | grep -E "Model Name|Processor"'
show 'machine'
show "ps -o pid=,command= -p $p | sed \"s|\$HOME|~|g\""
