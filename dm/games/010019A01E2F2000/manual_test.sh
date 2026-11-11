#!/bin/bash
# Manual test of the experimental internal scene resolution (README: Experimental 1440p internal scene rendering).
# Usage: manual_test.sh [1440p|1800p|2160p|default]   (default mode applies no runtime write: the comparison baseline)
# 1. --platform mac --seen-shaders: rewrites if needed (and makes main.runtime.tsv), builds the host from this
#    worktree and a partial shader library (shader_seen.tsv plus what earlier runs' host.log reported missing),
#    never the whole 260k-shader map. 2. Runs the game in a window with sound. Its save is the build folder's
#    run/save, not the installed app's. 3. After the game quits, prints a summary of its log (summary.txt).
# Never prints key material: prod.keys is passed by path only.
set -euo pipefail
MODE=${1:-1440p}
case $MODE in default|1440p|1800p|2160p|diag1792) ;; *) echo "mode: 1440p, 1800p, 2160p, diag1792 or default" >&2; exit 2;; esac
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
IMG=${MP4_IMAGES:-"$HOME/Downloads/Nintendo Switch 1:2 — images"}
NSP="$IMG/Metroid Prime 4 Beyond [NSZ]"; WORK="$NSP/010019A01E2F2000_build"
PY=${PY:-$( [ -x "$HOME/games/venv/bin/python" ] && echo "$HOME/games/venv/bin/python" || echo /opt/homebrew/bin/python3.14 )}   # the tool venv (capstone, lz4, fonttools, zbic: a rewrite needs them); the images folder's name has a ":", where venv refuses to go
OUT="$NSP/010019A01E2F2000-work/manual-test/$MODE-$(date +%m%d-%H%M%S)"; mkdir -p "$OUT"
# DUMP=every:<n> sets the pass-dump interval (every:150 catches a scan held for a few seconds); input.rec is player 1's input as the game's reads saw it (DM_REC), replayable with DM_KEYS=@<file>
printf 'DM_INTERNAL_RESOLUTION=%s\nDM_PRESENT_UPSCALE=off\nDM_METAL_HUD=1\nDM_NVN_PASSDUMP=%s\nDM_REC=%s\nDM_RUMBLE_LOG=1\n' "$MODE" "${DUMP:-every:1800}" "$OUT/input.rec" > "$OUT/env.txt"
[ -n "${ANISO:-}" ] && echo "DM_NVN_ANISO=$ANISO" >> "$OUT/env.txt"   # ANISO=<2..16>: the anisotropy floor of the surface samplers (DM_NVN_ANISO), for an A/B against a run without it
# PROBE=<module:offset,...>: guest probes (DM_PROBE, Mac), the first 3 hits of each printed; RUMBLE_PROBE=1 is the vibration set (README: Vibration with a DualSense):
# main:936628 the rumble setup runs, main:9366a0 its RumbleCategories lookup found the resource, main:a4e00 / main:d350b8 an amplitude or effect request
[ "${RUMBLE_PROBE:-}" = 1 ] && PROBE=main:936628,main:9366a0,main:a4e00,main:d350b8
# SCAN_PROBE=1: main:331d18, the scene-size function's path when the render-target pool refuses its second target (0x33187c)
[ "${SCAN_PROBE:-}" = 1 ] && PROBE=${PROBE:+$PROBE,}main:331d18
[ -n "${PROBE:-}" ] && printf 'DM_PROBE=%s\nDM_PROBE_LIMIT=3\n' "$PROBE" >> "$OUT/env.txt"
echo "== build (partial shaders; the first time a few thousand shaders compile) -> $OUT/build.log"
"$PY" "$REPO/scripts/build_ipa_from_nsp.py" "$NSP" -k "$IMG/prod.keys" --package v131072 --platform mac \
    --seen-shaders --work-dir "$WORK" --env-file "$OUT/env.txt" --progress lines 2>&1 | tee "$OUT/build.log"
CMD=$(grep -E "^cd .*dmhost.* \| tee " "$OUT/build.log" | tail -1)
[ -n "$CMD" ] || { echo "[RED] the build printed no run command: see $OUT/build.log" >&2; exit 1; }
echo "== running ($MODE). Play, then quit the game window."
bash -c "$CMD" || true
cp "$WORK/run/host.log" "$OUT/host.log"
"$PY" -I - "$OUT/host.log" "$MODE" "$OUT/input.rec" <<'PYEOF' | tee "$OUT/summary.txt"
import re, sys, collections
log, mode = open(sys.argv[1], errors='replace').read(), sys.argv[2]
print(f'mode {mode}; log {sys.argv[1]}')
try: rec = open(sys.argv[3]).read() if len(sys.argv) > 3 else ''
except OSError: rec = ''
spans = []   # ZL as the game's reads saw it: consecutive recorded steps holding ZL merged; a gap of 1-2 frames inside a hold would be a flicker
for f, keys, n in re.findall(r'f(\d+):([^~]*)~f(\d+)', rec):
    if 'ZL' not in keys.split('+'): continue
    f, n = int(f), int(n)
    if spans and f - spans[-1][1] <= 2: spans[-1][2] += f > spans[-1][1]; spans[-1][1] = max(spans[-1][1], f + n)
    else: spans.append([f, f + n, 0])
print(f'ZL holds seen by the game: {len(spans)}; longest {max((b - a for a, b, _ in spans), default=0)} frames; holds of 1 s or more: '
      + (', '.join(f'f{a}-f{b} ({(b - a) / 60:.1f} s, {g} short gaps)' for a, b, g in spans if b - a >= 60) or 'none') if rec else 'ZL holds: no input recording')
for l in re.findall(r'^.*(?:runtime patches|DM_NVN_ANISO:).*$', log, re.M)[:3]: print(l)
miss = {a or b for a, b in re.findall(r'\[RED\] (?:shader ([0-9a-f]{16}) \([^)]*\) failed to load:|failed to load shader ([0-9a-f]{16})_b:)', log)}   # the build's SHADER_MISSING: geometry shaders too
print(f'missing shaders: {len(miss)} distinct (the next run of this script compiles them)')
att = collections.Counter(re.findall(r'^\[pass\] F\d+ P \d+ (c0|d) ser=\d+ fmt=(\d+) (\d+x\d+)', log, re.M))
print('attachments (kind fmt size: passes):', ', '.join(f'{k} {f} {s}: {n}' for (k, f, s), n in att.most_common(10)))
vp = collections.Counter(re.findall(r'vp\[0\] = 0,0,(\d+,\d+)', log))
print('viewports:', ', '.join(f'{s.replace(",", "x")}: {n}' for s, n in vp.most_common(8)))
for l in re.findall(r'^.*timing up to frame.*$', log, re.M)[-3:]: print(l[:260])
for l in re.findall(r'^.*(?:pipeline prewarm list|pipeline prepare|pipeline archive /|pipeline prewarm done).*$', log, re.M)[:6]: print(l[:240])
gs = sorted(set(re.findall(r'draw with geometry shader ([0-9a-f]{16}), which has no library', log)))
print(f'draws whose geometry shader has no library: {gs or "none"}')
print('dual-source shaders without their entry point:', sorted(set(re.findall(r'shader ([0-9a-f]{16})_b writes output 1 and a pipeline blends', log))) or 'none')
lf = re.findall(r'\[longframe\] frame \d+ \(([\d.]+) s\) whole frame ([\d.]+) ms, submit\+present ([\d.]+) ms, GPU ([\d.]+) ms; new pipelines (\d+) \+ shader libraries \d+, ([\d.]+) ms in all; texture uploads \d+ first \+ \d+ re-uploads, [\d.]+ MB / ([\d.]+) ms', log)
cov = [float(t) for t in re.findall(r'\[longframe\] frame \d+ \(([\d.]+) s\)', log)]
kinds = collections.Counter(); worst = collections.defaultdict(float); npipe = 0
for t, w, s, g, n, pms, tms in lf:
    w, s, pms, tms = float(w), float(s), float(pms), float(tms); npipe += int(n)
    k = 'pipeline compile' if pms >= w / 2 else 'texture upload' if tms >= w * 0.3 else 'not in submit (window covered, or the game)' if s < w * 0.3 else 'submit, other'
    kinds[k] += 1; worst[k] = max(worst[k], w)
print(f'long frames: {len(lf)}, pipelines compiled in them {npipe};', '; '.join(f'{k} {n} (worst {worst[k]:.0f} ms)' for k, n in kinds.most_common()))
print('window covered (frame times then measure nothing):', len(re.findall(r'window covered', log)), 'times')
iv = [(int(a), float(b)) for a, b in re.findall(r'on-screen up to frame \d+: \d+ frames, dropped (\d+) \(discarded \d+\), p99 ([\d.]+)', log)]
if iv: print(f'on-screen windows of 300 frames: {len(iv)}, dropped in all {sum(d for d, _ in iv)}, windows with more than 6 dropped {sum(d > 6 for d, _ in iv)}')
rum = [(int(n), int(u), float(a)) for n, u, a in re.findall(r'rumble: (\d+) values sent, (\d+) with no output, peak amplitude ([\d.]+)', log)]
hand = re.findall(r'rumble: game asked for vibration handles of npad 0 style (\S+): ([^\n]*)', log)
print(f'vibration: handles for npad 0 {hand[:1] or "never asked"}; values {sum(n for n, _, _ in rum)}, {sum(u for _, u, _ in rum)} with no output; seconds with a nonzero amplitude {sum(a > 0 for _, _, a in rum)} of {len(rum)}, peak {max((a for _, _, a in rum), default=0):.2f}')
probes = {}
for name, n in re.findall(r'^\[dm\] PROBE (\S+) #(\d+)', log, re.M): probes[name] = max(probes.get(name, 0), int(n))
if probes or re.search(r'^\[dm\] probe \S+ \(original instruction', log, re.M): print('probe hits (highest hit number seen; 3 printed each):', probes or 'none')
red = collections.Counter(re.sub(r'[0-9a-f]{8,}|\d+', 'N', l)[:160] for l in re.findall(r'^.*\[RED\].*$', log, re.M) if 'failed to load shader' not in l)
print('other red lines:'); [print(f'  {n} x {l}') for l, n in red.most_common(8)]
for l in re.findall(r'^.*(?:SIGSEGV|SIGBUS|fault at|crash handler).*$', log, re.M)[:3]: print('CRASH:', l[:200])
PYEOF
echo "== summary: $OUT/summary.txt"
