#!/bin/sh
# Runs the host unit tests of a CMake build folder (dm/host/CMakeLists.txt) and records the run in the repository: one log per test and summary.txt in
# logs/<platform>/ (logs/README.md). For Linux (in the Docker image, docker/README.md) and for a cross-built Windows tree in Git Bash on the PC
# (dm/host/tests/run_win64_tests.ps1 is the PowerShell equivalent). From anywhere:
#   sh dm/host/tests/run_host_tests.sh <build folder> [<log folder>]
# The runs come from <build folder>/host_tests.tsv; <log folder> defaults to logs/<platform of the build> (linux, win64) at the repository root, and its old *.log files and
# summary.txt are removed first. Each test gets TIMEOUT seconds (default 120, the CTest limit). Exit status: 0 when every run passed, 1 when one did not, 2 on a usage error
set -u
usage() { echo "usage: sh dm/host/tests/run_host_tests.sh <build folder> [<log folder>]" >&2; exit 2; }
[ $# -ge 1 ] && [ $# -le 2 ] || usage
invoked="sh $0 $*"
started_in=$(pwd)
here=$(cd "$(dirname "$0")" && pwd) || exit 2
repo=$(cd "$here/../../.." && pwd) || exit 2
build=$(cd "$1" 2>/dev/null && pwd) || { echo "no build folder: $1" >&2; exit 2; }
manifest=$build/host_tests.tsv
[ -f "$manifest" ] || { echo "$manifest is missing: configure and build the tree first (docker/README.md)" >&2; exit 2; }
platform=$(sed -n 's/^# platform: //p' "$manifest" | head -n 1)
logs=${2:-$repo/logs/${platform:-unknown}}
mkdir -p "$logs" && logs=$(cd "$logs" && pwd) || exit 2
rm -f "$logs"/*.log "$logs/summary.txt"
limit=${TIMEOUT:-120}

# A Windows tree in Git Bash: the test programs are native Windows programs, which take C:/... paths in TMPDIR (Git Bash converts no variable under MSYS_NO_PATHCONV=1)
native_build=$build
if [ "$platform" = win64 ] && (cd "$build" && pwd -W) >/dev/null 2>&1; then native_build=$(cd "$build" && pwd -W); fi
tmpdir=$native_build/tmp
# GNU timeout (Linux, Git Bash); a Windows timeout.exe found first on PATH takes other arguments, so it is tried before use
wrap= timed=
if timeout 5 true >/dev/null 2>&1; then wrap="timeout $limit"; timed=1; fi
# Line-buffered output where stdbuf works (glibc programs), so a test that crashes still leaves the lines it printed in its log
if [ "$platform" = linux ] && stdbuf -oL -eL true >/dev/null 2>&1; then wrap="$wrap stdbuf -oL -eL"; fi

now() { date -u +%Y-%m-%dT%H:%M:%SZ; }
commit=$(git -C "$repo" rev-parse HEAD 2>/dev/null) || commit=unknown
if [ -n "$(git -C "$repo" status --porcelain --untracked-files=no -- . ':(exclude)logs' 2>/dev/null | head -n 1)" ]; then commit="$commit (with uncommitted changes)"; fi
started=$(now)

tab=$(printf '\t')
results=$build/.run_host_tests.$$
: > "$results"
total=0 passed=0 failed=0
while IFS="$tab" read -r name exe args tmp; do
    case $name in ''|'#'*) continue ;; esac
    total=$((total + 1))
    log=$logs/$name.log
    argv=; [ "$args" = - ] || argv=" $args"
    env=; [ "$tmp" = tmpdir ] && { mkdir -p "$build/tmp"; env="TMPDIR=$tmpdir "; }
    cmdline="cd $native_build && $env${wrap:+$wrap }./$exe$argv"
    printf 'date: %s\ncommit: %s\ncommand: %s\n\n' "$(now)" "$commit" "$cmdline" > "$log"
    t0=$(date +%s)
    # $wrap and the arguments are split at spaces on purpose
    (cd "$build" && if [ "$tmp" = tmpdir ]; then TMPDIR=$tmpdir; export TMPDIR; fi; exec $wrap "./$exe" $([ "$args" = - ] || echo "$args")) >> "$log" 2>&1
    rc=$?
    secs=$(($(date +%s) - t0))
    printf '\nexit: %s (%s s)\n' "$rc" "$secs" >> "$log"
    if [ "$rc" -eq 0 ]; then line="PASS $name ($secs s)"; passed=$((passed + 1))
    elif [ -n "$timed" ] && [ "$rc" -eq 124 ]; then line="TIMEOUT $name (killed after $limit s; see $name.log)"; failed=$((failed + 1))
    else line="FAIL $name (exit $rc, $secs s; see $name.log)"; failed=$((failed + 1)); fi
    echo "$line"; echo "$line" >> "$results"
done < "$manifest"

total_line="$total runs: $passed passed, $failed failed"
{
    printf 'date: %s\ncommit: %s\ncommand: %s\n' "$started" "$commit" "$invoked"
    printf 'directory: %s\nhost: %s\nbuild folder: %s\n' "$started_in" "$(uname -srm 2>/dev/null || echo unknown)" "$native_build"
    [ -n "$timed" ] || echo "timeout: none (no GNU timeout on PATH): a hanging test stops the run"
    sed -n '2,$s/^# //p' "$manifest"
    echo
    cat "$results"
    echo
    echo "$total_line"
} > "$logs/summary.txt"
rm -f "$results"
echo "$total_line (logs in $logs)"
[ "$total" -gt 0 ] && [ "$failed" -eq 0 ]
