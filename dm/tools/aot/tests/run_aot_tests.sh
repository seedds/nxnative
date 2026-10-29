#!/bin/sh
# Runs the tests of a CMake build folder of dm/tools/aot (the vector harness, the census test, the vectors-current check, discovery over the corpus,
# the manifest test, the corpus reference run, the corpus recompiled and run natively, the per-function differential test) and records the run in the repository: one log per test and summary.txt in logs/aot/ (logs/README.md).
# In the nxnative-aot Docker image (dm/tools/aot/README.md). From anywhere:
#   sh dm/tools/aot/tests/run_aot_tests.sh <build folder> [<log folder>]
# <log folder> defaults to logs/aot at the repository root; its old *.log files and summary.txt are removed first. Each test gets TIMEOUT seconds
# (default 900). Exit status: 0 when every test passed, 1 when one did not, 2 on a usage error
set -u
usage() { echo "usage: sh dm/tools/aot/tests/run_aot_tests.sh <build folder> [<log folder>]" >&2; exit 2; }
[ $# -ge 1 ] && [ $# -le 2 ] || usage
invoked="sh $0 $*"
started_in=$(pwd)
here=$(cd "$(dirname "$0")" && pwd) || exit 2
aot=$(cd "$here/.." && pwd) || exit 2
repo=$(cd "$aot/../../.." && pwd) || exit 2
build=$(cd "$1" 2>/dev/null && pwd) || { echo "no build folder: $1" >&2; exit 2; }
for t in nxaot nxaot_vec nxaot_run nxaot_host nxaot_funcdiff nxaot_bench; do
    [ -x "$build/$t" ] || { echo "$build has no $t: configure and build it first (dm/tools/aot/README.md)" >&2; exit 2; }
done
logs=${2:-$repo/logs/aot}
mkdir -p "$logs" && logs=$(cd "$logs" && pwd) || exit 2
rm -f "$logs"/*.log "$logs/summary.txt"
limit=${TIMEOUT:-900}
wrap=
if timeout 5 true >/dev/null 2>&1; then wrap="timeout $limit"; fi

now() { date -u +%Y-%m-%dT%H:%M:%SZ; }
commit=$(git -C "$repo" rev-parse HEAD 2>/dev/null) || commit=unknown
if [ -n "$(git -C "$repo" status --porcelain --untracked-files=no -- . ':(exclude)logs' 2>/dev/null | head -n 1)" ]; then commit="$commit (with uncommitted changes)"; fi
started=$(now)
results=$logs/.results.$$
: > "$results"
total=0 passed=0 failed=0

# run <name> <command...>: one test, its output in <name>.log
run() {
    name=$1; shift
    total=$((total + 1))
    log=$logs/$name.log
    printf 'date: %s\ncommit: %s\ncommand: %s\n\n' "$(now)" "$commit" "$*" > "$log"
    t0=$(date +%s)
    # $wrap is split at its space on purpose
    (cd "$repo" && exec $wrap "$@") >> "$log" 2>&1
    rc=$?
    secs=$(($(date +%s) - t0))
    printf '\nexit: %s (%s s)\n' "$rc" "$secs" >> "$log"
    if [ "$rc" -eq 0 ]; then line="PASS $name ($secs s)"; passed=$((passed + 1))
    elif [ "$rc" -eq 124 ] && [ -n "$wrap" ]; then line="TIMEOUT $name (killed after $limit s; see $name.log)"; failed=$((failed + 1))
    else line="FAIL $name (exit $rc, $secs s; see $name.log)"; failed=$((failed + 1)); fi
    echo "$line"; echo "$line" >> "$results"
}

run vectors "$build/nxaot_vec" "$aot/tests/vectors"
run vectors_v3 "$build/nxaot_vec" --cpu x86-64-v3 "$aot/tests/vectors/crypto.tsv.gz"
run vectors_perop "$build/nxaot_vec" --nan-mode perop "$aot/tests/vectors/fp.tsv.gz" "$aot/tests/vectors/fp_seq.tsv.gz"
run opcodes_reach "$build/nxaot" opcodes --reach --check
run census python3 "$aot/tests/test_census.py" --nxaot "$build/nxaot" --work "$build/census"
run vectors_current python3 "$aot/tests/gen_vectors.py" --check
run discover python3 "$aot/tests/test_discover.py" --nxaot "$build/nxaot" --work "$build/discover"
run manifest python3 "$aot/tests/test_manifest.py" --nxaot "$build/nxaot" --work "$build/manifest"
run corpus_ref python3 "$aot/tests/test_corpus_run.py" --nxaot-run "$build/nxaot_run"
run corpus_fp_current python3 "$aot/corpus/gen_fp.py" --check
run corpus_native python3 "$aot/tests/test_corpus_native.py" --nxaot "$build/nxaot" --nxaot-host "$build/nxaot_host" --nxaot-run "$build/nxaot_run" --work "$build/native"
run corpus_funcdiff python3 "$aot/tests/test_funcdiff.py" --nxaot "$build/nxaot" --nxaot-funcdiff "$build/nxaot_funcdiff" --work "$build/funcdiff"
run opcodes "$build/nxaot" opcodes
run bench "$build/nxaot_bench" --iterations 200000 --work "$build/bench"

total_line="$total runs: $passed passed, $failed failed"
{
    printf 'date: %s\ncommit: %s\ncommand: %s\n' "$started" "$commit" "$invoked"
    printf 'directory: %s\nhost: %s\nbuild folder: %s\n' "$started_in" "$(uname -srm 2>/dev/null || echo unknown)" "$build"
    "$build/nxaot" version | sed 's/^/toolchain: /'
    printf 'compiler: %s\n' "$(c++ --version 2>/dev/null | head -n 1)"
    for t in vectors vectors_v3 vectors_perop; do grep -h '^== nxaot_vec' "$logs/$t.log" 2>/dev/null | sed "s/^/$t: /"; done
    grep -h '^nxaot_vec: LLVM\|^nxaot_vec: lowering' "$logs/vectors.log" 2>/dev/null
    grep -h '^# reach' "$logs/opcodes_reach.log" 2>/dev/null
    grep -h '^census test:' "$logs/census.log" 2>/dev/null
    grep -h '^discover test:' "$logs/discover.log" 2>/dev/null
    grep -h '^manifest test:\|^manifest rows:\|^behaviour images:' "$logs/manifest.log" 2>/dev/null
    grep -h '^corpus reference run:' "$logs/corpus_ref.log" 2>/dev/null
    grep -h '^corpus native run:' "$logs/corpus_native.log" 2>/dev/null
    grep -h '^corpus per-function differential test' "$logs/corpus_funcdiff.log" 2>/dev/null
    grep -h '^# ' "$logs/opcodes.log" 2>/dev/null | sed 's/^# /lifter opcodes: /'
    echo
    cat "$results"
    echo
    echo "$total_line"
} > "$logs/summary.txt"
rm -f "$results"
echo "$total_line (logs in $logs)"
[ "$failed" -eq 0 ]
