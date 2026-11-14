#!/bin/sh
# Runs the host unit tests of a CMake build folder (dm/host/CMakeLists.txt) and records the run in the repository: one log per test and summary.txt in
# logs/<platform>/ (logs/README.md). For Linux (in the Docker image, docker/README.md) and for a cross-built Windows tree in Git Bash on the PC
# (dm/host/tests/run_win64_tests.ps1 is the PowerShell equivalent). From anywhere:
#   sh dm/host/tests/run_host_tests.sh <build folder> [<log folder>]
# The runs come from <build folder>/host_tests.tsv (the test programs), then <build folder>/host_scripts.tsv (the tests that are scripts: Python drivers, shell scripts; a skip
# code ends them as SKIP, which does not fail the run; NXAOT=<path> in the environment reaches t_second_pack, which skips without it); <log folder> defaults to logs/<platform of the build> (linux, win64) at the repository root, and its old *.log files and
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

# The AOT tests take their packs from NXAOT_PACKS (dm/tools/aot/tests/build_packs.sh's output; on a Windows tree <build>/packs/win64 when it exists, as
# run_win64_tests.ps1 takes it), and t_aot_host the checkout's corpus: the path compiled in is the container's. Without packs those tests pass with a note
packs=${NXAOT_PACKS:-}
if [ -z "$packs" ] && [ "$platform" = win64 ] && [ -d "$build/packs/win64" ]; then packs=$native_build/packs/win64; fi
[ -z "$packs" ] || { NXAOT_PACKS=$packs; export NXAOT_PACKS; }
corpus=$repo/dm/tools/aot/corpus
if [ "$platform" = win64 ] && (cd "$corpus" && pwd -W) >/dev/null 2>&1; then corpus=$(cd "$corpus" && pwd -W); fi

tab=$(printf '\t')
results=$build/.run_host_tests.$$
: > "$results"
total=0 passed=0 failed=0
while IFS="$tab" read -r name exe args tmp; do
    case $name in ''|'#'*) continue ;; esac
    total=$((total + 1))
    log=$logs/$name.log
    argv=; [ "$args" = - ] || argv=" $args"
    if [ -n "$packs" ] && [ "$name" = t_aot_host ]; then args="$([ "$args" = - ] || echo "$args") --corpus $corpus"; argv=" $args"; fi
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

# The script tests (<build folder>/host_scripts.tsv, host_script_test() in dm/host/CMakeLists.txt: Python drivers and shell scripts), after the programs, each with its own log in
# the same form. python3 is $PYTHON when set; {build} and {repo} in the arguments and the environment are this machine's folders; the rest of the environment is the runner's
# (NXAOT=<path> reaches t_second_pack this way). An exit with the row's skip code is SKIP, quoted with the script's own skip line, and does not fail the run
scripts=$build/host_scripts.tsv
native_repo=$repo
if [ "$platform" = win64 ] && (cd "$repo" && pwd -W) >/dev/null 2>&1; then native_repo=$(cd "$repo" && pwd -W); fi
subst() { printf '%s' "$1" | sed -e "s|{build}|$native_build|g" -e "s|{repo}|$native_repo|g"; }
s_total=0 s_passed=0 s_failed=0 s_skipped=0 s_reads= s_note=
if [ ! -f "$scripts" ]; then
    s_note="script tests: none run (no host_scripts.tsv: the tree was configured before dm/host/CMakeLists.txt wrote it; configure it again)"
else
    while IFS="$tab" read -r name interp script args envs reads skip; do
        case $name in ''|'#'*) continue ;; esac
        s_total=$((s_total + 1))
        log=$logs/$name.log
        case $interp in python3) prog=${PYTHON:-python3} ;; *) prog=$interp ;; esac
        [ "$args" = - ] && args=
        [ "$envs" = - ] && envs=
        [ "$reads" = - ] && reads=
        args=$(subst "$args"); envs=$(subst "$envs")
        missing=
        for v in $reads; do
            case $v in *[!A-Za-z0-9_]*) continue ;; esac
            eval "val=\${$v:-}"
            if [ -n "$val" ]; then case " $s_reads " in *" $v=$val "*) ;; *) s_reads="$s_reads $v=$val" ;; esac
            else missing="$missing $v"; case " $s_reads " in *" $v not set "*) ;; *) s_reads="$s_reads $v not set" ;; esac; fi
        done
        cmdline="cd $native_build && ${envs:+$envs }${wrap:+$wrap }$prog $native_repo/$script${args:+ $args}"
        printf 'date: %s\ncommit: %s\ncommand: %s\n\n' "$(now)" "$commit" "$cmdline" > "$log"
        t0=$(date +%s)
        # The environment and the arguments are split at spaces on purpose, without globbing; "" is an empty argument
        (
            set -f
            cd "$build" || exit 2
            for kv in $envs; do
                export "$kv"
                case $kv in TMPDIR=*) mkdir -p "${kv#TMPDIR=}" ;; esac
            done
            set --
            for a in $args; do [ "$a" = '""' ] && a=; set -- "$@" "$a"; done
            exec $wrap "$prog" "$native_repo/$script" "$@"
        ) >> "$log" 2>&1
        rc=$?
        secs=$(($(date +%s) - t0))
        said=$(sed -n '5,$p' "$log" | grep -i 'skip' | tail -n 1)
        [ -n "$said" ] || said=$(sed -n '5,$p' "$log" | grep -v '^[[:space:]]*$' | tail -n 1)
        printf '\nexit: %s (%s s)\n' "$rc" "$secs" >> "$log"
        if [ "$rc" -eq 0 ]; then line="PASS $name ($secs s)"; s_passed=$((s_passed + 1))
        elif [ "$skip" != - ] && [ "$rc" -eq "$skip" ]; then
            line="SKIP $name (exit $rc, $secs s${missing:+;$(for v in $missing; do printf ' %s not set' "$v"; done)}): \"$said\""; s_skipped=$((s_skipped + 1))
        elif [ -n "$timed" ] && [ "$rc" -eq 124 ]; then line="TIMEOUT $name (killed after $limit s; see $name.log)"; s_failed=$((s_failed + 1))
        else line="FAIL $name (exit $rc, $secs s; see $name.log)"; s_failed=$((s_failed + 1)); fi
        echo "$line"; echo "$line" >> "$results"
    done < "$scripts"
fi
[ -n "$s_note" ] && echo "$s_note"

total_line="$total runs: $passed passed, $failed failed"
scripts_line="$s_total script tests: $s_passed passed, $s_failed failed, $s_skipped skipped"
{
    printf 'date: %s\ncommit: %s\ncommand: %s\n' "$started" "$commit" "$invoked"
    [ -z "$packs" ] || echo "packs: $packs"
    [ -z "$s_reads" ] || echo "script tests' environment:$s_reads"
    printf 'directory: %s\nhost: %s\nbuild folder: %s\n' "$started_in" "$(uname -srm 2>/dev/null || echo unknown)" "$native_build"
    [ -n "$timed" ] || echo "timeout: none (no GNU timeout on PATH): a hanging test stops the run"
    sed -n '2,$s/^# //p' "$manifest"
    echo
    cat "$results"
    [ -z "$s_note" ] || echo "$s_note"
    echo
    echo "$total_line"
    echo "$scripts_line"
} > "$logs/summary.txt"
rm -f "$results"
echo "$total_line; $scripts_line (logs in $logs)"
[ "$total" -gt 0 ] && [ "$failed" -eq 0 ] && [ "$s_failed" -eq 0 ]
