#!/bin/sh
# hung.sh - running rosetta processes: CPU time, state, plus a sampling profile of the host (sample, 2 s)
ps -axo pid,stat,time,%cpu,command | grep "rosetta \./" | grep -v grep
p=$(ps -axo pid,command | grep "rosetta \./" | grep -v grep | head -1 | awk '{print $1}')
[ -n "$p" ] && sample $p 2 -mayDie 2>/dev/null | grep -E "^ +[0-9]+ " | head -25
