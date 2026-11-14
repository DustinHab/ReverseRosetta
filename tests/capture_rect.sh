#!/bin/sh
# capture_rect.sh X Y W H FILE - (through in_session.sh) capture exactly this screen rectangle as PNG
R=$(cd "$(dirname "$0")/.." && pwd)
$R/tools/screen_grab $1 $2 $3 $4 $5 | tail -1
