#!/bin/sh
# jobs.sh - running test jobs (in_session, rosetta, probe scripts)
ps -axo pid,etime,command | grep -E "session|rosetta|app_probe|graphics_check" | grep -v grep
