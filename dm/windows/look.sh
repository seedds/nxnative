#!/bin/bash
# look.sh <dir> <name> <wait> <keys...>: press, wait, shot, and wait until it is written
R=$1; n=$2; w=$3; shift 3; for k in "$@"; do echo "$k" >> "$R/ctl"; sleep 1.5; done; sleep "$w"; rm -f "$R/shots/$n.png"; echo "shot $R/shots/$n.png" >> "$R/ctl"; for i in $(seq 1 60); do [ -s "$R/shots/$n.png" ] && break; sleep 1; done; sleep 0.5; ls -l "$R/shots/$n.png" 2>/dev/null | awk '{print $5}'
