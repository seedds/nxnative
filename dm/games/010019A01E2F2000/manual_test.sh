#!/bin/bash
# Manual test of the experimental internal scene resolution (README: Experimental 1440p internal scene rendering).
# Usage: manual_test.sh [1440p|default]   (default mode applies no runtime write: the comparison baseline)
# 1. --platform mac --seen-shaders: rewrites if needed (and makes main.runtime.tsv), builds the host from this
#    worktree and a partial shader library (shader_seen.tsv plus what earlier runs' host.log reported missing),
#    never the whole 260k-shader map. 2. Runs the game in a window with sound. Its save is the build folder's
#    run/save, not the installed app's. 3. After the game quits, prints a summary of its log (summary.txt).
# Never prints key material: prod.keys is passed by path only.
set -euo pipefail
MODE=${1:-1440p}
case $MODE in default|1440p) ;; *) echo "mode: 1440p or default" >&2; exit 2;; esac
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
IMG=${MP4_IMAGES:-"$HOME/Downloads/Nintendo Switch 1:2 — images"}
NSP="$IMG/Metroid Prime 4 Beyond [NSZ]"; WORK="$NSP/010019A01E2F2000_build"
PY=${PY:-/opt/homebrew/bin/python3.14}
OUT="$NSP/010019A01E2F2000-work/manual-test/$MODE-$(date +%m%d-%H%M%S)"; mkdir -p "$OUT"
printf 'DM_INTERNAL_RESOLUTION=%s\nDM_PRESENT_UPSCALE=off\nDM_METAL_HUD=1\nDM_NVN_PASSDUMP=every:1800\n' "$MODE" > "$OUT/env.txt"
echo "== build (partial shaders; the first time a few thousand shaders compile) -> $OUT/build.log"
"$PY" "$REPO/scripts/build_ipa_from_nsp.py" "$NSP" -k "$IMG/prod.keys" --package v131072 --platform mac \
    --seen-shaders --work-dir "$WORK" --env-file "$OUT/env.txt" --progress lines 2>&1 | tee "$OUT/build.log"
CMD=$(grep -E "^cd .*dmhost.* \| tee " "$OUT/build.log" | tail -1)
[ -n "$CMD" ] || { echo "[RED] the build printed no run command: see $OUT/build.log" >&2; exit 1; }
echo "== running ($MODE). Play, then quit the game window."
bash -c "$CMD" || true
cp "$WORK/run/host.log" "$OUT/host.log"
"$PY" -I - "$OUT/host.log" "$MODE" <<'PYEOF' | tee "$OUT/summary.txt"
import re, sys, collections
log, mode = open(sys.argv[1], errors='replace').read(), sys.argv[2]
print(f'mode {mode}; log {sys.argv[1]}')
for l in re.findall(r'^.*runtime patches.*$', log, re.M)[:2]: print(l)
miss = set(re.findall(r'failed to load shader ([0-9a-f]{16})_b', log))
print(f'missing shaders: {len(miss)} distinct (the next run of this script compiles them)')
att = collections.Counter(re.findall(r'^\[pass\] F\d+ P \d+ (c0|d) ser=\d+ fmt=(\d+) (\d+x\d+)', log, re.M))
print('attachments (kind fmt size: passes):', ', '.join(f'{k} {f} {s}: {n}' for (k, f, s), n in att.most_common(10)))
vp = collections.Counter(re.findall(r'vp\[0\] = 0,0,(\d+,\d+)', log))
print('viewports:', ', '.join(f'{s.replace(",", "x")}: {n}' for s, n in vp.most_common(8)))
for l in re.findall(r'^.*timing up to frame.*$', log, re.M)[-3:]: print(l[:260])
red = collections.Counter(re.sub(r'[0-9a-f]{8,}|\d+', 'N', l)[:160] for l in re.findall(r'^.*\[RED\].*$', log, re.M) if 'failed to load shader' not in l)
print('other red lines:'); [print(f'  {n} x {l}') for l, n in red.most_common(8)]
for l in re.findall(r'^.*(?:SIGSEGV|SIGBUS|fault at|crash handler).*$', log, re.M)[:3]: print('CRASH:', l[:200])
PYEOF
echo "== summary: $OUT/summary.txt"
