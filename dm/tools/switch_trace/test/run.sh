#!/bin/sh
# Builds nvn_trace.cpp against the mock NVN driver (mock_nvn.cpp) and checks each capture mode (check.py). Needs only clang++ and python3: ./run.sh from anywhere.
set -e
cd "$(dirname "$0")"
out=${TMPDIR:-/tmp}/switch-trace-test; rm -rf "$out"; mkdir -p "$out/sd/switch-trace"
clang++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Werror -DST_MOCK -o "$out/mock" ../src/nvn_trace.cpp mock_nvn.cpp
run() {   # run <case> <config lines...>
    case=$1; shift
    rm -f "$out"/sd/switch-trace/*; printf '%s\n' "$@" > "$out/sd/switch-trace/config.txt"
    ST_SD="$out/sd" "$out/mock" 200 2> "$out/$case.log" || { cat "$out/$case.log"; exit 1; }
    grep '^\[bench\]' "$out/$case.log" | sed "s/^/$case /" || true
    python3 check.py "$out/sd/switch-trace" "$case"
}
run timing-run mode=timing,seq frames=40 delay=5 key_skip=0x778:0x1a
python3 check.py "$out/sd/switch-trace" seq
cp "$out/sd/switch-trace/capture1.tsv" "$out/timing.tsv"; cp "$out/sd/switch-trace/seq1.tsv" "$out/seq.tsv"   # for join.py / seqdiff.py by hand
run timing-each mode=timing split=each frames=40 delay=5 key_skip=0x778:0x1a
run dump-key mode=dump,seq frames=5 delay=5 "dump_keys=$(python3 check.py --key ms)"
run dump-all mode=dump frames=3 delay=5 dump_all=1 dump_size=720x405
