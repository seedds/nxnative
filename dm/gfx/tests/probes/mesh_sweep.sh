#!/bin/bash
# usage: mesh_sweep.sh <dir of <key>_b.metal / _b.json from dmsh_build.py> <file of keys, one per line> <out.tsv> [parallel jobs, default 6]
# Every listed mesh or task shader, instrumented by mesh_guard.py (a guard band past its stage array), one draw on zeroed inputs (mesh_sweep), first under
# Metal shader validation (device memory), and without it when a validation compile takes longer than VTIMEOUT seconds (default 60; the largest shaders take minutes).
# Task shaders run one at a time: a GPU hang makes the GPU discard the other processes' work. Columns: key, stage, result, validation report (if any).
here=$(cd "$(dirname "$0")" && pwd); d=$1; keys=$2; tsv=$3; jobs=${4:-6}
out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
one() {
    k=$1; d=$2; out=$3; here=$4
    [ -f "$d/${k}_b.json" ] || { echo -e "$k\t-\tMISSING"; return; }
    cp "$d/${k}_b.json" "$out/"
    g=$(python3 "$here/mesh_guard.py" "$d/${k}_b.metal" "$out/${k}_b.metal" 2>&1) || { echo -e "$k\t-\tNOGUARD $g"; return; }
    res=$(MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 timeout "${VTIMEOUT:-60}" "$here/mesh_sweep" "$out" "$k" 2> "$out/$k.stderr"); rc=$?
    if [ $rc -eq 124 ]; then res=$(timeout 120 "$here/mesh_sweep" "$out" "$k" 2>/dev/null); [ $? -eq 124 ] && res="$k - TIMEOUT" || res="$res (no validation: it timed out compiling)"; fi
    val=$(grep -m1 -E "Invalid|out of bounds|frame #0" "$out/$k.stderr" | tr -s ' \t' ' ' | cut -c1-160)
    echo -e "$(echo "$res" | awk '{print $1"\t"$2"\t"substr($0, index($0,$3))}')\t${val:+VALIDATION: $val}"
}
export -f one
: > "$tsv"
while read -r k; do s=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get('stage',''))" "$d/${k}_b.json" 2>/dev/null); [ "$s" = ts ] && echo "$k" >> "$out/tasks" || echo "$k" >> "$out/meshes"; done < "$keys"
[ -f "$out/meshes" ] && xargs -P "$jobs" -I{} bash -c 'one "$@"' _ {} "$d" "$out" "$here" < "$out/meshes" >> "$tsv"
[ -f "$out/tasks" ] && xargs -P 1 -I{} bash -c 'one "$@"' _ {} "$d" "$out" "$here" < "$out/tasks" >> "$tsv"
echo "$(wc -l < "$tsv") shaders"; cut -f3 "$tsv" | awk '{print $1}' | sort | uniq -c
