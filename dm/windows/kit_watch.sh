#!/bin/bash
# kit_watch.sh <user@pc> <ssh options in one string> <short name> <title ID> <kit folder>: start it with a build on the PC. When the PC's log shows the shader split finished, it fetches the profile the PC
# derived or guessed, compiles the host here (kit_host.py) and sends it to C:\dmb\kits\<short name>, so the host is there when the build reaches that step.
set -u; H=$1; O=$2; n=$3; id=$4; K=$5; HERE=$(cd "$(dirname "$0")" && pwd); ok=''
for _ in $(seq 1 2880); do
  s=$(ssh $O "$H" 'if exist C:\dmb\results\build_console.log findstr /c:"shader split (" /c:"Traceback" C:\dmb\results\build_console.log' 2>/dev/null | tr -d '\r' | head -1)
  case "$s" in *Traceback*) echo "the PC stopped early"; exit 1;; esac
  ok=$(ssh $O "$H" "if exist C:\\dmb\\w-$n\\profile\\$id\\game.tsv echo yes" 2>/dev/null | tr -d '\r')
  [ -n "$s" ] && [ "$ok" = yes ] && break; sleep 10
done
[ "$ok" = yes ] || { echo "no profile from the PC in time"; exit 1; }
rm -rf "$K/profile"; mkdir -p "$K/profile/$id"; scp -q $O "$H:C:/dmb/w-$n/profile/$id/*" "$K/profile/$id/" || exit 1
python3 "$HERE/kit_host.py" "$K/profile" "$id" "$K" || exit 1
scp -q $O "$K/dmhost" "$H:C:/dmb/kits/$n/dmhost" && echo "$n host sent"
