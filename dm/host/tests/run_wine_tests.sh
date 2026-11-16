#!/bin/sh
# Runs the test programs of a cross-built Windows tree (build/win64: docker/README.md) under Wine on Linux, in the nxnative-wine image (docker/Dockerfile.wine), and records
# the run in logs/wine/ (logs/README.md): the closest check of the Windows programs a Linux machine gives. A run under Wine is not a run on Windows: summary.txt says so
# and names the Wine version on its first lines, and logs/win64/ stays for a real Windows PC. From anywhere, on the Linux host (Docker, no Wine needed on the host):
#   sh dm/host/tests/run_wine_tests.sh [--packs <folder>] [--image <name>] <build folder> [<log folder>]
# The list, the logs and the per-test environment are run_host_tests.sh's, which this script runs inside the image with WINE set (the programs through the image's
# wine, WineHQ's wine-stable on PATH, the paths they take in Wine's Z: form) and NOT_RUN set (the runs the table below marks not-run for this Wine). The container gets Xvfb
# (Wine's Vulkan needs a graphics driver: Dockerfile.wine), a fresh Wine prefix in its /tmp, a tmpfs at /run/user/<uid> (Wine's server directory: without it Wine 9.0
# aborted at start when TMPDIR was set, which the tmpdir tests need) and Wine's crash dialog off (a crash prints winedbg's backtrace into the test's log instead of waiting on
# a window).
# --packs: the Windows packs of the AOT tests (build_packs.sh's <out>/win64: the DLLs, module/, hostfn/, second_pack/), else NXAOT_PACKS, else <build folder>/packs/win64
# when it exists; mounted read-only. TIMEOUT=<seconds> per test (default 120).
# Known Wine differences: dm/host/tests/wine_differences.tsv (test, kind, check, evidence, reason, the Wine versions it holds for; its header has the rule). A FAIL or
# TIMEOUT whose every failed check (a line with FAIL or a cross mark) is one a `differs` row of the test names, with that row's evidence in the log, is reported as DIFFERS
# with the rows' reasons, apart from passes and failures; a test of a `not-run` row is not started (NOT RUN, with the reason). So a new failure stays a FAIL.
# Exit status: 0 when nothing failed (DIFFERS and NOT RUN do not fail the run), 1 when a run failed, 2 on a usage error, a missing tree or a missing image
set -u
usage() { echo "usage: sh dm/host/tests/run_wine_tests.sh [--packs <folder>] [--image <name>] <build folder> [<log folder>]" >&2; exit 2; }
image=nxnative-wine
packs=${NXAOT_PACKS:-}
invoked="sh $0 $*"
while [ $# -gt 0 ]; do
    case $1 in
        --packs) [ $# -ge 2 ] || usage; packs=$2; shift 2 ;;
        --image) [ $# -ge 2 ] || usage; image=$2; shift 2 ;;
        -h|--help) usage ;;
        -*) echo "unknown option: $1" >&2; usage ;;
        *) break ;;
    esac
done
[ $# -ge 1 ] && [ $# -le 2 ] || usage
here=$(cd "$(dirname "$0")" && pwd) || exit 2
repo=$(cd "$here/../../.." && pwd) || exit 2
build=$(cd "$1" 2>/dev/null && pwd) || { echo "no build folder: $1" >&2; exit 2; }
manifest=$build/host_tests.tsv
[ -f "$manifest" ] || { echo "$manifest is missing: cross-build the tree first (docker/README.md)" >&2; exit 2; }
platform=$(sed -n 's/^# platform: //p' "$manifest" | head -n 1)
[ "$platform" = win64 ] || { echo "$build is not a Windows tree (host_tests.tsv: platform ${platform:-unknown}); run_host_tests.sh runs a Linux tree" >&2; exit 2; }
logs=${2:-$repo/logs/wine}
mkdir -p "$logs" && logs=$(cd "$logs" && pwd) || exit 2
if [ -z "$packs" ] && [ -d "$build/packs/win64" ]; then packs=$build/packs/win64; fi
if [ -n "$packs" ]; then packs=$(cd "$packs" 2>/dev/null && pwd) || { echo "no packs folder: $packs (dm/tools/aot/tests/build_packs.sh writes <out>/win64)" >&2; exit 2; }; fi
known=$here/wine_differences.tsv
[ -f "$known" ] || { echo "$known is missing" >&2; exit 2; }
docker image inspect "$image" >/dev/null 2>&1 || { echo "no image $image: docker build -t nxnative-wine -f docker/Dockerfile.wine docker (docker/README.md)" >&2; exit 2; }
limit=${TIMEOUT:-120}
uid=$(id -u) gid=$(id -g)

# Inside the image: the display, the prefix, then run_host_tests.sh. Its arguments: repo, build, logs, packs (may be empty), the not-run list, the time limit
inner='set -u
repo=$1 build=$2 logs=$3 packs=$4 notrun=$5 limit=$6
wine=wine   # /opt/wine-stable/bin/wine of WineHQ, on the PATH of the image (Dockerfile.wine): the one loader of Wine 11 for 32- and 64-bit programs
# fixme-dbghelp: the symbol loader of winedbg writes thousands of "elf_search_auxv" lines into the log of a crashed test; the other fixme and err lines of Wine stay
export WINEPREFIX=/tmp/wineprefix DISPLAY=:99 PYTHONDONTWRITEBYTECODE=1 WINEDEBUG=fixme-dbghelp
mkdir -p "$WINEPREFIX" /tmp/nx-wine
Xvfb :99 -screen 0 1280x720x24 -nolisten tcp > /tmp/nx-wine/xvfb.txt 2>&1 &
i=0; while [ ! -S /tmp/.X11-unix/X99 ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
[ -S /tmp/.X11-unix/X99 ] || { echo "Xvfb did not start:"; cat /tmp/nx-wine/xvfb.txt; exit 3; }
"$wine" --version > /tmp/nx-wine/version.txt 2>&1
if ! "$wine" wineboot -i > /tmp/nx-wine/wineboot.txt 2>&1; then echo "wineboot failed:"; cat /tmp/nx-wine/wineboot.txt; exit 3; fi
"$wine" reg add "HKCU\\Software\\Wine\\WineDbg" /v ShowCrashDialog /t REG_DWORD /d 0 /f > /dev/null 2>&1 || { echo "the crash dialog could not be turned off"; exit 3; }
wineserver -p
# the not-run rows that hold for this Wine (the first word of its --version, or any)
ver=$(awk "{ print \$1; exit }" /tmp/nx-wine/version.txt)
awk -F "\t" -v ver="$ver" "\$1 !~ /^#/ && \$2 == \"not-run\" && (\$6 == \"any\" || index(\" \" \$6 \" \", \" \" ver \" \")) { print \$1 \"\t\" \$5 }" "$notrun" > /tmp/nx-wine/not_run.tsv
WINE=$wine NOT_RUN=/tmp/nx-wine/not_run.tsv TIMEOUT=$limit NXAOT_PACKS=$packs sh "$repo/dm/host/tests/run_host_tests.sh" "$build" "$logs"
rc=$?
wineserver -k 2>/dev/null
cp /tmp/nx-wine/version.txt "$logs/.wine_version"
exit $rc'

# Every folder at its own path in the container, so the paths are the same inside and out; DM_REPO for t_pipe, which otherwise looks for the checkout above the build
# folder (a tree built outside the checkout has none there)
set -- run --rm --name "nx-wine-run-$$" -u "$uid:$gid" -e HOME=/tmp -e "DM_REPO=Z:$repo" --tmpfs "/run/user/$uid:uid=$uid,gid=$gid,mode=700" \
    -v "$repo:$repo" -v "$build:$build" -v "$logs:$logs"
# The commit: line of the logs: a git worktree's .git names the main checkout's git folder, mounted read-only
common=$(git -C "$repo" rev-parse --path-format=absolute --git-common-dir 2>/dev/null) || common=
case $common in ''|"$repo"/*) ;; *) set -- "$@" -v "$common:$common:ro" ;; esac
[ -z "$packs" ] || set -- "$@" -v "$packs:$packs:ro"
set -- "$@" -w "$repo" "$image" sh -c "$inner" inner "$repo" "$build" "$logs" "$packs" "$known" "$limit"
started=$(date -u +%Y-%m-%dT%H:%M:%SZ)
rm -f "$logs/.wine_version"
docker "$@"
rc=$?
[ "$rc" -le 1 ] && [ -f "$logs/summary.txt" ] || { echo "the run under Wine did not complete (docker exit $rc)" >&2; exit 1; }
version=$(head -n 1 "$logs/.wine_version" 2>/dev/null); rm -f "$logs/.wine_version"
[ -n "$version" ] || version=unknown

# The known differences: each FAIL or TIMEOUT of the summary checked against the table's differs rows for its name that hold for this Wine (their last column lists
# the first word of `wine --version` of each Wine they were seen on, or any; wine_differences.tsv's header has the rule). A failed check is a line of the test's output
# (after the log's three header lines) with FAIL or a cross mark, Wine's own debug lines (0123:fixme:...) aside. Prints the reasons of the rows that explain the run;
# fails when a failed check is not explained, or nothing is
ver=${version%% *}
classify() {
    [ -f "$logs/$1.log" ] || return 1
    awk -F '\t' -v n="$1" -v ver="$ver" '
        FNR == NR { if ($1 == n && $2 == "differs" && ($6 == "any" || index(" " $6 " ", " " ver " "))) { k++; check[k] = $3; ev[k] = $4; why[k] = $5 " (holds for " $6 ")" } next }
        FNR > 4 { text[++lines] = $0 }
        END {
            if (!k) exit 1
            for (i = 1; i <= k; i++) { shown[i] = ev[i] == "-"; for (l = 1; l <= lines && !shown[i]; l++) if (index(text[l], ev[i])) shown[i] = 1 }
            nf = 0
            for (l = 1; l <= lines; l++) {
                s = text[l]
                if (s ~ /^[0-9a-f]+:(fixme|err|warn|trace):/ || !(index(s, "FAIL") || index(s, "\342\234\227"))) continue
                nf++; ok = 0
                for (i = 1; i <= k && !ok; i++) if (check[i] != "-" && shown[i] && index(s, check[i])) { ok = 1; used[i] = 1 }
                if (!ok) exit 1
            }
            if (!nf) for (i = 1; i <= k; i++) if (check[i] == "-" && ev[i] != "-" && shown[i]) used[i] = 1
            out = ""
            for (i = 1; i <= k; i++) if (used[i] && index(out, why[i]) == 0) out = out (out == "" ? "" : "; ") why[i]
            if (out == "") exit 1
            print out
        }' "$known" "$logs/$1.log"
}
out=$logs/.summary.$$
passed=0 failed=0 differs=0 skipped=0 notrun=0
{
    echo "Wine, not Windows: the Windows programs of this tree ran under $version on Linux (the $image image, docker/Dockerfile.wine), not on Windows"
    echo "wine: $version"
    echo "runner: $invoked (started $started)"
    echo "known Wine differences: dm/host/tests/wine_differences.tsv"
    echo
    sed -n '1,/^$/p' "$logs/summary.txt"
} > "$out"
results=$logs/.results.$$
: > "$results"
# A program that passed by skipping its checks (its last line says "skipped", as the t_vk_* programs do without a Vulkan device) is a SKIP here, quoted
said_last() { sed -n '5,$p' "$logs/$1.log" 2>/dev/null | grep -v -e '^exit: ' -e '^[0-9a-f]*:\(fixme\|err\|warn\|trace\):' | grep -v '^[[:space:]]*$' | tail -n 1; }
sed -n '/^$/,$p' "$logs/summary.txt" | while IFS= read -r line; do
    case $line in
        'PASS '*)
            name=${line#PASS }; name=${name%% *}; last=$(said_last "$name")
            case $last in *skipped*) echo "SKIP ${line#PASS }: \"$last\"" ;; *) echo "$line" ;; esac ;;
        'SKIP '*) echo "$line" ;;
        'NOT RUN '*) echo "$line" ;;
        'FAIL '*|'TIMEOUT '*)
            name=${line#* }; name=${name%% *}
            if why=$(classify "$name"); then echo "DIFFERS ${line#* }: under Wine: $why"; else echo "$line"; fi ;;
        *' runs: '*|*' script tests: '*|'') ;;
        *) echo "$line" ;;
    esac
done > "$results"
passed=$(grep -c '^PASS ' "$results"); failed=$(grep -c -e '^FAIL ' -e '^TIMEOUT ' "$results"); differs=$(grep -c '^DIFFERS ' "$results")
skipped=$(grep -c '^SKIP ' "$results"); notrun=$(grep -c '^NOT RUN ' "$results")
total=$((passed + failed + differs + skipped + notrun))
{
    cat "$results"
    echo
    sed -n 's/^\([0-9]* runs: .*\)$/run_host_tests.sh: \1/p; s/^\([0-9]* script tests: .*\)$/run_host_tests.sh: \1/p' "$logs/summary.txt"
    echo "under Wine ($version), $total tests: $passed passed, $failed failed, $differs differ under Wine, $skipped skipped, $notrun not run"
} >> "$out"
mv "$out" "$logs/summary.txt"; rm -f "$results"
grep -e '^FAIL ' -e '^TIMEOUT ' -e '^DIFFERS ' "$logs/summary.txt"
echo "under Wine ($version), $total tests: $passed passed, $failed failed, $differs differ under Wine, $skipped skipped, $notrun not run (logs in $logs)"
[ "$failed" -eq 0 ]
