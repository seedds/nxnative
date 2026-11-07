#!/bin/zsh
# drive.sh <results dir> <app> <max seconds>: one game run driven through <dir>/ctl; ends at the limit or when <dir>/stop appears; the save is backed up and restored
R=$1; app=$2; secs=$3; id=${DRIVE_ID:-01007B600D5BC000}; L=~/Library/Logs/nxnative/$id.log; SV=~/Library/Application\ Support/nxnative/$id/save; BK="$R/save_backup"
rm -rf "$R"; mkdir -p "$R/shots"; : > "$R/ctl"; had=0; [ -d "$SV" ] && [ -n "$(ls -A "$SV" 2>/dev/null)" ] && { ditto "$SV" "$BK"; had=1; }; touch "$R/marker"; sleep 1
DM_LAUNCHER=0 DM_CTL="$R/ctl" "$app/Contents/MacOS/nxnative" --no-launcher > "$R/stdout" 2>&1 &
pid=$!; t=0
while [ $t -lt $secs ] && kill -0 $pid 2>/dev/null && [ ! -e "$R/stop" ]; do sleep 5; t=$((t+5)); done
if kill -0 $pid 2>/dev/null; then echo "alive at end: yes ($t s)" > "$R/result.txt"; else echo "alive at end: NO, ended near $t s" > "$R/result.txt"; fi
kill $pid 2>/dev/null; sleep 5; hp=$(pgrep -P $pid dmhost); [ -n "$hp" ] && { kill -9 $hp; sleep 2; echo "host force-stopped" >> "$R/result.txt"; }; kill -9 $pid 2>/dev/null
[ "$L" -nt "$R/marker" ] && cp "$L" "$R/log.txt"
if [ $had = 1 ]; then rm -rf "$SV"; ditto "$BK" "$SV"; echo 'save: restored' >> "$R/result.txt"; elif [ -d "$SV" ]; then mv "$SV" "$R/test_save"; echo 'save: test save moved aside' >> "$R/result.txt"; fi
echo DRIVE-DONE
