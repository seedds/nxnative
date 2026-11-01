#!/usr/bin/env python3
# One session's frame-rate report from the host's per-frame timing rows (DM_NVN_TSV): a timeline per second and every drop below the cap with what limited the frames there.
# Usage: python3 dm/tools/perf_report.py <frames.tsv> [--every S] [--all]
#   Per second: fps, frame / CPU / GPU time (p50 and p95, ms), the hold (@itv), draws and passes, and, from the system profile (dm/host/sysprof.m, Mac): the GPU clock, the share
#   of time the GPU was powered, its power, the boost controller's state, thermal and voltage-droop limits, the process role, thermal state and Low Power Mode.
#   Drops: runs of seconds below 0.9 of the cap, each with the second before it, and a verdict from those columns:
#     cpu      the frame's CPU time (the present thread, @cpu_ms) alone does not fit the frame
#     gpu-limit the GPU time does not fit and a thermal or droop limit applied, or the thermal state was serious or Low Power Mode on
#     gpu-clock the GPU time does not fit at a clock in the lower third of its range with no limit applied: the performance controller chose the clock
#     gpu      the GPU time does not fit at a high clock: the work itself is too much for this GPU
#     role     the process was not the foreground app (role other than 1) during the drop
#     pacing   neither CPU nor GPU time explains the frame time (waits, presentation)
#   A/B: with DM_AB=<frames> and switches set to ab, frames carry @ab (the switches on in their phase, bit i = the i-th ab switch the log names): each set is compared over the same
#   session, without the first 300 frames after each switch
#   --every S: seconds per timeline row (default 1); --all: print every row, not only the minute around each drop
import argparse, statistics, sys
from collections import defaultdict

ROLES = {0: 'unspecified', 1: 'foreground', 2: 'background', 3: 'control', 4: 'graphics server', 5: 'throttled', 6: 'non-UI', 7: 'default', 8: 'darwin bg', -1: '?'}
COLS = ['@frame_ms', '@cpu_ms', '@gpu_ms', '@itv', '@draws', '@passes', '@gpu_mhz', '@gpu_on', '@gpu_w', '@gpu_boost', '@gpu_cltm', '@gpu_droop', '@role', '@thermal', '@low_power', '@call_ms', '@ab']


def load(path):
    rows = defaultdict(dict)
    with open(path) as f:
        for line in f:
            p = line.rstrip('\n').split('\t')
            if len(p) != 3 or p[1] not in COLS: continue
            try: rows[int(p[0])][p[1]] = float(p[2])
            except ValueError: continue
    return [dict(v, frame=k) for k, v in sorted(rows.items()) if '@call_ms' in v and '@frame_ms' in v]


def q(v, p):
    if not v: return None
    v = sorted(v); return v[min(len(v) - 1, int(p * len(v)))]


def bucket(frames, every):
    out = []; t0 = frames[0]['@call_ms'] / 1000
    by = defaultdict(list)
    for r in frames: by[int((r['@call_ms'] / 1000 - t0) // every)].append(r)
    for k in sorted(by):
        fr = by[k]; col = lambda c, fr=fr: [r[c] for r in fr if c in r and r[c] >= 0]
        b = {'t': k * every, 'n': len(fr), 'fps': len(fr) / every, 'first': fr[0]['frame']}
        for c in ('@frame_ms', '@cpu_ms', '@gpu_ms'): b[c] = (q(col(c), .5), q(col(c), .95))
        for c in ('@itv', '@draws', '@passes', '@gpu_mhz', '@gpu_on', '@gpu_w', '@gpu_boost', '@gpu_cltm', '@gpu_droop', '@thermal', '@low_power'):
            v = col(c); b[c] = statistics.mean(v) if v else None
        roles = col('@role'); b['@role'] = statistics.mode(roles) if roles else None
        out.append(b)
    return out


def fmt(x, d=0):
    return '-' if x is None else f'{x:.{d}f}'


def verdict(b, budget, fmin, fmax):
    if b['@role'] is not None and b['@role'] != 1: return 'role'
    cpu, gpu = b['@cpu_ms'][0], b['@gpu_ms'][0]
    if cpu is not None and cpu > budget: return 'cpu'
    if gpu is not None and gpu > budget * 0.95:
        if (b['@gpu_cltm'] or 0) > 1 or (b['@gpu_droop'] or 0) > 1 or (b['@thermal'] or 0) >= 2 or (b['@low_power'] or 0) > 0.5: return 'gpu-limit'
        mhz = b['@gpu_mhz']
        if mhz is not None and fmax > fmin and mhz < fmin + (fmax - fmin) / 3: return 'gpu-clock'
        return 'gpu'
    return 'pacing'


def main():
    ap = argparse.ArgumentParser(description='Frame-rate report of one session from DM_NVN_TSV rows')
    ap.add_argument('tsv'); ap.add_argument('--every', type=float, default=1.0); ap.add_argument('--all', action='store_true')
    a = ap.parse_args()
    frames = load(a.tsv)
    if not frames: sys.exit('no frame rows with @call_ms (DM_NVN_TSV from this host?)')
    bs = bucket(frames, a.every)
    cap = max(round(statistics.median(b['fps'] for b in bs) / 30) * 30, 30) if bs else 60
    budget = 1000 / cap
    mhz = [b['@gpu_mhz'] for b in bs if b['@gpu_mhz']]
    fmin, fmax = (min(mhz), max(mhz)) if mhz else (0, 0)
    has_sys = bool(mhz)
    print(f'{len(frames)} frames over {bs[-1]["t"] + a.every:.0f} s; cap {cap} fps (budget {budget:.1f} ms); GPU clock seen {fmt(fmin)}–{fmt(fmax)} MHz'
          + ('' if has_sys else ' (no system profile in this file: a host without dm/host/sysprof.m)'))
    hdr = f'{"t s":>6} {"fps":>5} {"frame p50/95":>13} {"cpu p50/95":>11} {"gpu p50/95":>11} {"hold":>4} {"draws":>5} {"pass":>4} {"MHz":>5} {"on%":>4} {"W":>5} {"boost":>5} {"cltm":>4} {"droop":>5} {"role":>10} {"th":>2} {"lpm":>3}  verdict'
    drops, cur = [], None
    for i, b in enumerate(bs):
        low = b['fps'] < 0.9 * cap
        if low:
            if cur is None: cur = [i, i]
            else: cur[1] = i
        elif cur is not None: drops.append(cur); cur = None
    if cur is not None: drops.append(cur)
    show = set(range(len(bs))) if a.all else {j for s, e in drops for j in range(max(0, s - 30), min(len(bs), e + 10))}

    def row(b):
        low = b['fps'] < 0.9 * cap
        return (f'{b["t"]:6.0f} {b["fps"]:5.1f} {fmt(b["@frame_ms"][0],1):>6}/{fmt(b["@frame_ms"][1],1):<6} {fmt(b["@cpu_ms"][0],1):>5}/{fmt(b["@cpu_ms"][1],1):<5} {fmt(b["@gpu_ms"][0],1):>5}/{fmt(b["@gpu_ms"][1],1):<5}'
                f' {fmt(b["@itv"],1):>4} {fmt(b["@draws"]):>5} {fmt(b["@passes"]):>4} {fmt(b["@gpu_mhz"]):>5} {fmt(b["@gpu_on"]):>4} {fmt(b["@gpu_w"],2):>5} {fmt(b["@gpu_boost"],1):>5}'
                f' {fmt(b["@gpu_cltm"]):>4} {fmt(b["@gpu_droop"]):>5} {ROLES.get(b["@role"], str(b["@role"])) if b["@role"] is not None else "-":>10} {fmt(b["@thermal"]):>2} {fmt(b["@low_power"]):>3}'
                + (f'  {verdict(b, budget, fmin, fmax)}' if low else ''))
    print('\nTimeline' + ('' if a.all else ' (around the drops; --all for every second)')); print(hdr)
    last = -2
    for i, b in enumerate(bs):
        if i not in show: continue
        if i != last + 1 and last >= 0: print('   ...')
        print(row(b)); last = i
    print(f'\nDrops below {0.9 * cap:.0f} fps: {len(drops)}')
    tally = defaultdict(float)
    for s, e in drops:
        seg = bs[s:e + 1]; vs = [verdict(b, budget, fmin, fmax) for b in seg]
        for v in vs: tally[v] += a.every
        before = bs[s - 1] if s > 0 else None
        print(f'  {seg[0]["t"]:.0f}–{seg[-1]["t"] + a.every:.0f} s (frame {seg[0]["first"]}): {min(b["fps"] for b in seg):.0f}–{max(b["fps"] for b in seg):.0f} fps; verdict {"/".join(sorted(set(vs)))}'
              + (f'; the second before: GPU {fmt(before["@gpu_ms"][0],1)} ms at {fmt(before["@gpu_mhz"])} MHz, CPU {fmt(before["@cpu_ms"][0],1)} ms' if before else ''))
    if tally: print('Seconds below the cap by verdict: ' + ', '.join(f'{k} {v:.0f} s' for k, v in sorted(tally.items(), key=lambda x: -x[1])))
    ab(frames, cap)
    if has_sys:
        at60 = [b for b in bs if b['fps'] >= 0.95 * cap and b['@gpu_mhz']]
        if at60: print(f'At the cap: GPU {fmt(statistics.median(b["@gpu_ms"][0] for b in at60 if b["@gpu_ms"][0]),1)} ms at {fmt(statistics.median(b["@gpu_mhz"] for b in at60))} MHz, powered {fmt(statistics.median(b["@gpu_on"] for b in at60))}% (median of seconds)')



def ab(frames, cap, settle=300):   # DM_AB: each set of switches over its frames, without the first frames after each switch
    if len({r.get('@ab') for r in frames if '@ab' in r}) < 2: return
    groups, prev, since = defaultdict(list), None, 0
    for r in frames:
        m = r.get('@ab')
        since = since + 1 if m == prev else 0; prev = m
        if m is not None and since >= settle: groups[m].append(r)
    print(f'\nA/B (DM_AB phases by the switches on, mask as in the log\'s order; each switch\'s first {settle} frames left out)')
    print(f'{"mode":>10} {"frames":>7} {"fps":>5} {"late %":>6} {"frame p50/95":>13} {"gpu p50/95":>11} {"MHz p50":>7} {"on% p50":>7} {"W p50":>5}')
    for m in sorted(groups):
        g = groups[m]; col = lambda c, g=g: [r[c] for r in g if c in r and r[c] >= 0]
        fr = col('@frame_ms'); secs = sum(fr) / 1000 or 1
        late = 100 * sum(1 for x in fr if x > 1.25 * 1000 / cap) / max(len(fr), 1)
        med = lambda c, d=0, col=col: fmt(q(col(c), .5), d)
        print(f'{"base" if not m else f"mask {int(m)}":>10} {len(g):7d} {len(fr) / secs:5.1f} {late:6.1f} {med("@frame_ms",1):>6}/{fmt(q(fr, .95),1):<6} {med("@gpu_ms",1):>5}/{fmt(q(col("@gpu_ms"), .95),1):<5} {med("@gpu_mhz"):>7} {med("@gpu_on"):>7} {med("@gpu_w",2):>5}')


if __name__ == '__main__': main()
