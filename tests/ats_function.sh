#!/bin/sh
# ats_function.sh FUNCTION [LINES] - function from the saved Intel ATS disassembly (/tmp/ats_all.txt)
awk -v f="$1:" -v n="${2:-80}" '$0 == f { p = 1 } p { print; if (++k > n) exit }' /tmp/ats_all.txt
