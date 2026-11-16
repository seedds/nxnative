#!/bin/sh
# Runs the host unit tests of a CMake build folder (dm/host/CMakeLists.txt) and records the run in the repository: one log per test and summary.txt in
# logs/<platform>/ (logs/README.md). For Linux (in the Docker image, docker/README.md) and for a cross-built Windows tree in Git Bash on the PC
# (dm/host/tests/run_win64_tests.ps1 is the PowerShell equivalent). From anywhere:
#   sh dm/host/tests/run_host_tests.sh <build folder> [<log folder>]
# The runs come from <build folder>/host_tests.tsv (the test programs), then <build folder>/host_scripts.tsv (the tests that are scripts: Python drivers, shell scripts; a skip
# code ends them as SKIP, which does not fail the run; NXAOT=<path> in the environment reaches t_second_pack, which skips without it); <log folder> defaults to logs/<platform of the build> (linux, win64) at the repository root, and its old *.log files and
# summary.txt are removed first. Each test gets TIMEOUT seconds (default 120, the CTest limit). Exit status: 0 when every run passed, 1 when one did not, 2 on a usage error.
# Under Wine (dm/host/tests/run_wine_tests.sh runs this script in the nxnative-wine image), two more variables, which change nothing when unset: WINE=<the wine loader> runs
# each program of a Windows tree through it, with the paths the programs take in Wine's Z: form (Z: is /); the script tests, whose interpreter is then a Linux one, get
# the Unix paths and WINE in their environment. NOT_RUN=<file> (lines: name<TAB>reason) names runs that are not started: each is a NOT RUN line with its reason, neither
# passed nor failed
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
# A Windows tree under Wine: WINE names the loader (a Windows tree only); the programs take Z:/... paths
wine=; [ "$platform" = win64 ] && wine=${WINE:-}
[ -z "$wine" ] || native_build=Z:$build
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
# run_win64_tests.ps1 takes it), and t_aot_host and t_aot_targets the checkout's corpus: the path compiled in is the container's. Without packs those tests pass with a note
packs=${NXAOT_PACKS:-}
if [ -z "$packs" ] && [ "$platform" = win64 ] && [ -d "$build/packs/win64" ]; then packs=$native_build/packs/win64; fi
# Under Wine the script tests read the packs by their Unix path, the programs by the Z: one
script_packs=$packs
if [ -n "$wine" ]; then case $packs in /*) packs=Z:$packs ;; Z:/*) script_packs=${packs#Z:} ;; esac; fi
[ -z "$packs" ] || { NXAOT_PACKS=$packs; export NXAOT_PACKS; }
corpus=$repo/dm/tools/aot/corpus
if [ "$platform" = win64 ] && (cd "$corpus" && pwd -W) >/dev/null 2>&1; then corpus=$(cd "$corpus" && pwd -W); fi
[ -z "$wine" ] || corpus=Z:$corpus
# The runs not to start (NOT_RUN=<file>: name<TAB>reason); prints the reason of a listed name
not_run() { [ -n "${NOT_RUN:-}" ] && awk -F '\t' -v n="$1" '$1 == n { print $2; found = 1; exit } END { exit !found }' "$NOT_RUN"; }
skipped_runs=0

tab=$(printf '\t')
results=$build/.run_host_tests.$$
: > "$results"
total=0 passed=0 failed=0
while IFS="$tab" read -r name exe args tmp; do
    case $name in ''|'#'*) continue ;; esac
    if why=$(not_run "$name"); then line="NOT RUN $name: $why"; skipped_runs=$((skipped_runs + 1)); echo "$line"; echo "$line" >> "$results"; continue; fi
    total=$((total + 1))
    log=$logs/$name.log
    argv=; [ "$args" = - ] || argv=" $args"
    if [ -n "$packs" ] && { [ "$name" = t_aot_host ] || [ "$name" = t_aot_targets ]; }; then args="$([ "$args" = - ] || echo "$args") --corpus $corpus"; argv=" $args"; fi
    # TMPDIR for the tmpdir tests; under Wine also WINETMPDIR: Wine 11 imports a Unix TMPDIR into the Windows environment as WINE_HOST_TMPDIR (dlls/ntdll/unix/env.c,
    # is_special_env_var), and gives a program TMPDIR only from WINETMPDIR; Wine 9.0 imports TMPDIR as it is and WINETMPDIR as a variable of that name, which nothing reads
    env=; [ "$tmp" = tmpdir ] && { mkdir -p "$build/tmp"; env="TMPDIR=$tmpdir ${wine:+WINETMPDIR=$tmpdir }"; }
    cmdline="cd $native_build && $env${wrap:+$wrap }${wine:+$wine }./$exe$argv"
    printf 'date: %s\ncommit: %s\ncommand: %s\n\n' "$(now)" "$commit" "$cmdline" > "$log"
    t0=$(date +%s)
    # $wrap, $wine and the arguments are split at spaces on purpose
    (cd "$build" && if [ "$tmp" = tmpdir ]; then TMPDIR=$tmpdir; export TMPDIR; if [ -n "$wine" ]; then WINETMPDIR=$tmpdir; export WINETMPDIR; fi; fi
     exec $wrap $wine "./$exe" $([ "$args" = - ] || echo "$args")) >> "$log" 2>&1
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
# Under Wine the interpreters are Linux programs: the Unix paths
script_build=$native_build; [ -z "$wine" ] || script_build=$build
if [ -n "$wine" ] && [ -n "$script_packs" ]; then NXAOT_PACKS=$script_packs; export NXAOT_PACKS; fi
subst() { printf '%s' "$1" | sed -e "s|{build}|$script_build|g" -e "s|{repo}|$native_repo|g"; }
s_total=0 s_passed=0 s_failed=0 s_skipped=0 s_reads= s_note=
if [ ! -f "$scripts" ]; then
    s_note="script tests: none run (no host_scripts.tsv: the tree was configured before dm/host/CMakeLists.txt wrote it; configure it again)"
else
    while IFS="$tab" read -r name interp script args envs reads skip; do
        case $name in ''|'#'*) continue ;; esac
        if why=$(not_run "$name"); then line="NOT RUN $name: $why"; skipped_runs=$((skipped_runs + 1)); echo "$line"; echo "$line" >> "$results"; continue; fi
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
[ "$skipped_runs" -eq 0 ] || scripts_line="$scripts_line; $skipped_runs not run (NOT_RUN)"
{
    printf 'date: %s\ncommit: %s\ncommand: %s\n' "$started" "$commit" "$invoked"
    [ -z "$wine" ] || echo "wine: $wine (WINE): the programs ran under Wine"
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
