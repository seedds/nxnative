#!/usr/bin/env python3
# A repeatable benchmark of a game in the Mac host: the same save, the same scripted route (button presses and stick moves at fixed times from start), the same measuring window,
# and frame-time percentiles from the host's per-frame timing rows (DM_NVN_TSV): frame time (start to start), the interval each frame stayed on screen, GPU time, frames per second.
# Variants run the same route with other settings, in turn and repeated, so two hosts or two sets of switches compare under the same load; a screenshot after the measurement
# window of every run shows whether a variant changes the picture without stalling a measured frame.
# Usage: python3 scripts/bench.py <id>_build folder [options]   (the folder the NSP script made with --platform mac: tools/dmhost, the rewritten modules, the extracted RomFS,
#   shader/cache and font; the game's title ID is the folder name's start). -h lists the options. Exit code 0 when every run gave frames, 1 otherwise.
#   --route <name>: a route of the game's profile (dm/games/<id>/bench.tsv); the first one by default. --save <folder>: the save the route starts from (default <id>_build/run/save);
#   each run gets a copy, the folder itself is not changed. --variant 'label:KEY=VALUE,KEY=VALUE': a variant (repeatable; KEY host=<path> runs another host binary, msl=<folder>
#   other Mac shader libraries, shmap=<file> another shader map (with the c1_full.tsv / cb_extent.tsv of those libraries beside it), patched=<folder> other rewritten modules,
#   pso=<folder> another pipeline archive); without one, a single variant 'base'. --runs N: each variant N times, in turn (default 2).
#   --fps / --scale: DM_FPS_TARGET (with the display link at that rate) / DM_RENDER_SCALE for every variant; --env KEY=VALUE for every variant. --out <folder>: where the runs go
#   (default <id>_build/bench/<date-time>): per run log.txt, frames.tsv and the screenshots, and results.json and results.md for all.
# A route line in bench.tsv: name, DM_KEYS (seconds from host start), the window's start and length in seconds, a description. The window starts once the scene is loaded and the
# pipelines of the first frames are built, so runs compare the scene rather than its loading. Load on the Mac moves the numbers: compare variants of one invocation, not invocations.
import argparse, datetime, json, math, os, re, shutil, signal, statistics, subprocess, sys, time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'dm/tools'))
import romfs as rfs


def fail(msg):
    print(f'error: {msg}', file=sys.stderr); sys.exit(1)


def newest(paths):   # the newest version folder (v<number>) of a glob
    vs = sorted((p for p in paths if re.fullmatch(r'v\d+', p.name.removeprefix('patched_'))), key=lambda p: int(p.name.removeprefix('patched_')[1:]))
    return vs[-1] if vs else None


def routes(gid):
    f = REPO / 'dm/games' / gid / 'bench.tsv'
    if not f.is_file(): return {}
    out = {}
    for ln in f.read_text().splitlines():
        if not ln.strip() or ln.startswith('#'): continue
        c = ln.split('\t')
        if len(c) < 4: fail(f'{f}: a route line needs name, keys, window start and window length: {ln!r}')
        out[c[0]] = {'keys': c[1], 'from': float(c[2]), 'secs': float(c[3]), 'about': c[4] if len(c) > 4 else ''}
    return out


def pct(xs, p):   # nearest-rank percentile
    if not xs: return None
    s = sorted(xs); return s[min(len(s) - 1, max(0, math.ceil(p / 100 * len(s)) - 1))]


def frames(tsv, t0, t1):
    """The per-frame rows of one run (frame / @name / value), the frames whose @t_s lies in [t0, t1): frame time, on-screen interval, GPU time"""
    rows = {}
    with open(tsv, errors='replace') as fh:
        for ln in fh:
            c = ln.rstrip('\n').split('\t')
            if len(c) != 3 or not c[1].startswith('@'): continue
            try: rows.setdefault(int(c[0]), {})[c[1]] = float(c[2])
            except ValueError: continue
    sel = [r for f, r in sorted(rows.items()) if '@t_s' in r and t0 <= r['@t_s'] < t1]
    ft = [r['@frame_ms'] for r in sel if r.get('@frame_ms', 0) > 0]
    shown = [r['@presented_ms'] for r in sel if r.get('@presented_ms', 0) > 0]
    gpu = [r['@gpu_ms'] for r in sel if r.get('@gpu_ms', -1) >= 0]
    dropped = sum(1 for r in sel if r.get('@presented_ms') == -1)
    return ft, shown, gpu, dropped, len(sel)


def summary(ft, shown, gpu, dropped, n, secs, budget):
    d = {'frames': n, 'fps': round(n / secs, 1) if secs else None, 'dropped': dropped}
    for name, xs in (('frame', ft), ('shown', shown), ('gpu', gpu)):
        for p in (50, 95, 99): d[f'{name}_p{p}'] = round(pct(xs, p), 2) if xs else None
        d[f'{name}_max'] = round(max(xs), 2) if xs else None
    d['over_budget'] = sum(1 for x in ft if x > budget * 1.5)   # frames taking more than 1.5 times the target's frame time: the spikes the Metal HUD shows
    return d


def run_one(a, gid, build, var, route, out, lock):
    build, out = build.resolve(), out.expanduser().resolve()
    var = {k: str(Path(v).expanduser().resolve()) if k in ('host', 'patched', 'msl', 'shmap') else v for k, v in var.items()}
    env = dict(os.environ)
    host = Path(var.get('host') or build / 'tools/dmhost')
    patched = Path(var.get('patched') or newest(build.glob('patched_v*')) or fail(f'no rewritten modules (patched_v*) in {build}'))
    ver = patched.name.removeprefix('patched_')
    romfs = rfs.source(build / 'extract' / ver)
    if not romfs.is_dir(): romfs = rfs.source(newest(build.glob('extract/v*')))
    prof = build / 'profile' / gid if (build / 'profile' / gid / 'game.tsv').is_file() else REPO / 'dm/games' / gid
    out.mkdir(parents=True, exist_ok=True); save, temp = out / 'save', out / 'temp'
    shutil.rmtree(save, ignore_errors=True); shutil.copytree(a.save, save); temp.mkdir(exist_ok=True)
    env.update(DM_ROOT=str(build), DM_ROMFS=str(romfs), DM_SAVE=str(save), DM_TEMP=str(temp), DM_MSL_DIR=str(var.get('msl') or build / 'shader/cache'),
               DM_FONT_DIR=str(build / 'font'), DM_NVN_SHMAP=str(var.get('shmap') or prof / 'shader_map.tsv'), DM_WINDOW='1', DM_AUDIO='0', DM_KEYS=route['keys'],
               DM_NVN_TSV=str(out / 'frames.tsv'), DM_NVN_TSV_CALLS='0', DM_CTL=str(out / 'ctl'), DM_METAL_HUD='0' if a.no_hud else '1')
    if (build / 'aoc').is_dir(): env['DM_AOC_DIR'] = str(build / 'aoc')
    archive = var.get('pso') or a.pso_archive
    if archive: env['DM_PSO_ARCHIVE'] = archive if archive == 'off' else str(Path(archive).expanduser().resolve())
    if a.fps: env['DM_FPS_TARGET'] = str(a.fps)
    if a.fps and a.fps > 60: env.update(DM_PRESENT_HZ=str(a.fps), DM_PRESENT_LINK='2')   # the display link at the screen's rate for the variable frame step; at 60 or 30 the host matches the screen to the cap itself
    if a.scale: env['DM_RENDER_SCALE'] = str(a.scale)
    for kv in a.env: k, _, v = kv.partition('='); env[k] = v
    for k, v in var.items():
        if k not in ('host', 'msl', 'shmap', 'patched', 'pso'): env[k] = v
    (out / 'ctl').write_text('')
    end = route['from'] + route['secs'] + 2
    # Readback/PNG capture can stall a frame and change adaptive pacing. Keep the
    # default visual proof after the timing window, before the two-second exit tail.
    shots = sorted(a.shots) if a.shots else [route['from'] + route['secs'] + 0.5]
    p = None
    if lock: wait_lock(lock)
    try:
        with open(out / 'log.txt', 'w') as log:
            p = subprocess.Popen([str(host), str(patched)], cwd=out, env=env, stdout=log, stderr=subprocess.STDOUT)
        t0 = time.monotonic()
        while time.monotonic() - t0 < end and p.poll() is None:
            el = time.monotonic() - t0
            while shots and el >= shots[0]:
                with open(out / 'ctl', 'a') as c: c.write(f'shot {out}/shot_{int(shots.pop(0))}s.png\n')
            time.sleep(0.2)
    finally:
        if p is not None and p.poll() is None:
            p.send_signal(signal.SIGTERM)
            try: p.wait(5)
            except subprocess.TimeoutExpired: p.kill(); p.wait()
        if lock: lock.unlink(missing_ok=True)
    budget = 1000.0 / (a.fps or 60)
    s = summary(*frames(out / 'frames.tsv', route['from'], route['from'] + route['secs']), route['secs'], budget) if (out / 'frames.tsv').is_file() else {'frames': 0}
    log_text = (out / 'log.txt').read_text(errors='replace')
    m = re.findall(r'frame step: game time [\d.]+ s in [\d.]+ s \(([\d.]+) of real time\)', log_text)
    if m: s['game_speed'] = float(m[-1])
    s['red_lines'] = sum(1 for ln in log_text.splitlines() if '[RED]' in ln)
    return s


def lock_holder(lock):
    """The process ID a measuring lock names (its text: the ID alone, or '… pid <ID>'), when that process still runs; None for a free lock, one naming no ID (taken as held: 0),
    and a stale one: its process gone and no game running on this Mac"""
    try: text = Path(lock).read_text()
    except OSError: return None
    m = re.search(r'(?:^|pid )(\d+)\s*$', text.strip())
    if not m: return 0
    pid = int(m.group(1))
    try: os.kill(pid, 0); return pid
    except PermissionError: return pid
    except ProcessLookupError: pass
    return 0 if subprocess.run(['pgrep', '-x', 'dmhost'], capture_output=True).returncode == 0 else None


def wait_lock(lock, owner=''):   # the measuring lock other runs on this Mac hold while a game runs (an empty file name turns it off); taken atomically; a stale one is removed
    while True:
        try:
            fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY); os.write(fd, (f'{owner} pid {os.getpid()}' if owner else str(os.getpid())).encode()); os.close(fd); return
        except FileExistsError:
            if lock_holder(lock) is None:
                print(f'removing the stale measuring lock {lock} ({Path(lock).read_text(errors="replace").strip()[:80]}: its process is gone, no game runs)', flush=True)
                Path(lock).unlink(missing_ok=True); continue
            time.sleep(2)


def table(results, keys):
    head = ['variant', 'runs'] + keys
    rows = ['| ' + ' | '.join(head) + ' |', '|' + '---|' * len(head)]
    for label, rs in results.items():
        ok = [r for r in rs if r.get('frames')]
        def cell(k):
            values = [r[k] for r in ok if r.get(k) is not None]
            if not values: return '–'
            return f'{statistics.median(values):g}' + (f' ({min(values):g}–{max(values):g})' if len(values) > 1 else '')
        rows.append('| ' + ' | '.join([label, str(len(ok))] + [cell(k) for k in keys]) + ' |')
    return '\n'.join(rows)


def main():
    ap = argparse.ArgumentParser(description='Benchmark a game in the Mac host along a fixed route (see the comment at the top of this file).')
    ap.add_argument('build', help='the <id>_build folder of a --platform mac build')
    ap.add_argument('--route', help='a route name from dm/games/<id>/bench.tsv (default: its first)')
    ap.add_argument('--save', help='the save folder the route starts from (default <id>_build/run/save)')
    ap.add_argument('--variant', action='append', default=[], help="'label:KEY=VALUE,…' (repeatable); keys host, msl, shmap, patched, pso take paths")
    ap.add_argument('--runs', type=int, default=2)
    ap.add_argument('--fps', type=int, help='DM_FPS_TARGET for every variant (above 60 with the display link at that rate)')
    ap.add_argument('--scale', type=float, help='DM_RENDER_SCALE for every variant')
    ap.add_argument('--env', action='append', default=[], help='KEY=VALUE for every variant (repeatable)')
    ap.add_argument('--pso-archive', help='DM_PSO_ARCHIVE for every variant (the pipeline archive; default the host\'s own)')
    ap.add_argument('--lock', default='', help='a lock file to hold while a game runs, shared with other measuring runs on this Mac')
    ap.add_argument('--shots', type=lambda v: [float(x) for x in v.split(',')], help='screenshot times in seconds from host start, comma-separated (default: after the measurement window; in-window captures perturb timing)')
    ap.add_argument('--no-hud', action='store_true', help='no Metal performance HUD')
    ap.add_argument('--out', help='the output folder (default <id>_build/bench/<date-time>)')
    a = ap.parse_args()
    build = Path(a.build).expanduser().resolve()
    m = re.match(r'([0-9A-Fa-f]{16})_build$', build.name)
    if not m or not build.is_dir(): fail(f'{build} is not an <id>_build folder of the NSP script')
    gid = m.group(1).upper()
    rs = routes(gid)
    if a.route: route = rs.get(a.route) or fail(f'no route {a.route!r} in dm/games/{gid}/bench.tsv (routes: {", ".join(rs) or "none"})')
    elif rs: a.route, route = next(iter(rs.items()))
    else: fail(f'dm/games/{gid}/bench.tsv has no route')
    a.save = Path(a.save).expanduser() if a.save else build / 'run/save'
    if not a.save.is_dir(): fail(f'no save folder {a.save}')
    variants = {}
    for v in a.variant or ['base:']:
        label, _, kvs = v.partition(':')
        variants[label] = dict(kv.split('=', 1) for kv in kvs.split(',') if kv)
    out = Path(a.out).expanduser().resolve() if a.out else build / 'bench' / datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    lock = Path(a.lock) if a.lock else None
    print(f'{gid}, route {a.route!r} ({route["about"]}): window {route["from"]:g}–{route["from"] + route["secs"]:g} s; {len(variants)} variant(s) × {a.runs} run(s) → {out}', flush=True)
    results = {k: [] for k in variants}
    for i in range(a.runs):
        for label, var in variants.items():
            d = out / f'{label}-{i + 1}'
            s = run_one(a, gid, build, var, route, d, lock); results[label].append(s)
            print(f'  {label} run {i + 1}: ' + (f'{s["fps"]} fps, frame p50 {s["frame_p50"]} / p99 {s["frame_p99"]} / max {s["frame_max"]} ms, GPU p50 {s["gpu_p50"]} ms' if s.get('frames') else f'no frames (see {d / "log.txt"})'), flush=True)
    keys = ['fps', 'frame_p50', 'frame_p95', 'frame_p99', 'frame_max', 'shown_p99', 'shown_max', 'dropped', 'over_budget', 'gpu_p50', 'gpu_p95', 'game_speed', 'red_lines']
    md = (f'{gid}, route `{a.route}` ({route["about"]}), window {route["secs"]:g} s, fps target {a.fps or "profile"}, render scale {a.scale or "profile"}; '
          f'medians over runs (range); times in ms; over_budget = frames longer than 1.5 frame times\n\n' + table(results, keys) + '\n')
    (out / 'results.md').write_text(md); (out / 'results.json').write_text(json.dumps({'game': gid, 'route': a.route, **{k: v for k, v in vars(a).items() if k != 'build' and k != 'save'}, 'save': str(a.save), 'variants': variants, 'results': results}, indent=1, default=str))
    print('\n' + md)
    sys.exit(0 if all(r.get('frames') for rs_ in results.values() for r in rs_) else 1)


if __name__ == '__main__':
    main()
