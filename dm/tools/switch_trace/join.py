#!/usr/bin/env python3
"""Where the GPU time goes on the console and on the host, for the same game and scene: a switch_trace timing capture against a host run's GPU profile.

usage: join.py <console captureN.tsv> --log <host game.log> --frame F [--passes LO-HI] [--game <title id> | --shader-map <tsv>] [--top 25] [--host-tsv out.tsv]
       join.py <console captureN.tsv> --host <host.tsv> [...]             a host table written earlier with --host-tsv

The host run: DM_NVN_GPUPROF (every:<n> or a frame range) for the encoder times, and DM_NVN_PASSDUMP=F with DM_NVN_PASSDUMP_ALL=1 for frame F's shader keys ([pass] K lines
per render pass, X lines per dispatch). Each host encoder's time is the median over the profiled frames whose encoder at the same index has the same kind and size as in
frame F. --passes keeps only frame F's passes LO-HI (a ranged DM_NVN_GPUPROF window labels its frames by the first one; take LO-HI from F's [pass] P lines).
--game / --shader-map labels each key with its package, index and stage from the profile's shader_map.tsv.

Three tables:
  1. by shader key: each interval's (console) or encoder's (host) time split evenly over its keys, the console's median over its frames: ms on each side, each side's share
     of its frame, and host / console as times and as shares;
  2. by kind and size class (relative to each side's largest colour target, so different resolutions line up);
  3. "cheap on console, expensive here": keys ranked by how much more of the frame they take on the host (share ratio).
Compare shares, not milliseconds: clocks, resolution and dynamic resolution differ. Built on Pepper's OoT rank.py / join.py (2026-11-09)."""
import argparse, collections as C, csv, os, re, statistics as st, sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gpuprof  # noqa: E402  (dm/tools/gpuprof.py: the host's [gpuprof] line format)

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
K_LINE = re.compile(r'\[pass\] F(\d+) K (\d+) st(\d) key=([0-9a-f]{16})')
X_LINE = re.compile(r'\[pass\] F(\d+) X (\d+) dispatch ([0-9a-f]{16})\S* groups (\d+)x(\d+)x(\d+)')


def rows(path):
    with open(path) as f:
        return [r for r in csv.DictReader((l for l in f if not l.startswith('#')), delimiter='\t')]


def fnum(x, d=0.0):
    try: return float(x)
    except (TypeError, ValueError): return d


def host_table(log, frame, passes=None):
    """frame's encoders as rows {idx, us, kind, w, h, colors, draws, dispatches, keys}: the median time over the profiled frames, keys from frame's [pass] K / X lines"""
    with open(log, errors='replace') as f:
        lines = f.readlines()
    frames, _ = gpuprof.parse(lines)
    keys_r, keys_c = C.defaultdict(set), C.defaultdict(list)
    for ln in lines:
        m = K_LINE.search(ln)
        if m and int(m[1]) == frame: keys_r[int(m[2])].add(m[4]); continue
        m = X_LINE.search(ln)
        if m and int(m[1]) == frame: keys_c[int(m[2])].append(m[3])
    def unique(es):   # a second block under the same frame label (a frame the pass dump slowed) keeps its encoders under idx + 100000 * n
        out, seen = [], set()
        for e in es:
            i = e.idx
            while i in seen: i += 100000
            seen.add(i); out.append(e._replace(idx=i))
        return out
    frames = {f: unique(es) for f, es in frames.items()}
    kf = frames.get(frame)
    if not kf: sys.exit(f'{log}: no [gpuprof] encoder lines for frame {frame} (DM_NVN_GPUPROF must cover it)')
    if passes:
        lo, hi = passes
        kf = [e for e in kf if (lo <= e.pass_ <= hi if e.kind == 'R' else lo - 1 <= e.pass_ <= hi)]
        frames = {frame: kf}
    by_idx = {f: {e.idx: e for e in es} for f, es in frames.items()}
    out = []
    for e in kf:
        same = [by_idx[f][e.idx].us for f in by_idx if e.idx in by_idx[f] and by_idx[f][e.idx].kind == e.kind and (by_idx[f][e.idx].w, by_idx[f][e.idx].h) == (e.w, e.h)]
        ks = sorted(keys_r.get(e.pass_, ())) if e.kind == 'R' else sorted(set(keys_c.get(e.pass_, ())))
        out.append(dict(frame=frame, idx=e.idx, us=st.median(same), kind=e.kind, w=e.w, h=e.h, colors=e.colors, draws=e.draws, dispatches=e.dispatches, keys=','.join(ks)))
    return out


def shader_labels(path):
    labels = {}
    if not path or not os.path.exists(path): return labels
    for r in rows(path):
        k = (r.get('dm_ctl_sha16') or '')[:16]
        if k and k not in labels: labels[k] = f"{r.get('package', '')}#{r.get('index', '')} {r.get('stage', '')}".strip()
    return labels


def size_class(w, h, ref):
    if not w or not h: return 'none'
    s = ((w * h) / ref) ** 0.5 if ref else 1
    return 'full' if s >= 0.7 else 'half' if s >= 0.35 else 'quarter' if s >= 0.18 else 'eighth' if s >= 0.09 else 'small'


def attribute(rs):
    """{key: us}, {(kind, class): us}, total us for one frame's rows (console mesh intervals count as render, as the host's encoders do)"""
    keys, cls, tot = C.Counter(), C.Counter(), 0.0
    colour = [(int(fnum(r['w'])), int(fnum(r['h']))) for r in rs if r['kind'] in 'RM' and fnum(r.get('colors')) > 0]
    ref = max((w * h for w, h in colour), default=0)
    for r in rs:
        us = fnum(r['us'], -1)
        if us < 0: continue
        k = 'R' if r['kind'] == 'M' else r['kind']; tot += us
        ks = [x[:16] for x in (r.get('keys') or '').split(',') if x and x != '?']
        for x in ks: keys[x] += us / len(ks)
        cls[(k, size_class(int(fnum(r['w'])), int(fnum(r['h'])), ref) if k == 'R' else '-')] += us
    return keys, cls, tot


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('console'); ap.add_argument('--log'); ap.add_argument('--frame', type=int); ap.add_argument('--passes'); ap.add_argument('--host')
    ap.add_argument('--game'); ap.add_argument('--shader-map'); ap.add_argument('--top', type=int, default=25); ap.add_argument('--host-tsv')
    a = ap.parse_args(argv)
    con = [r for r in rows(a.console) if r.get('kind') in ('R', 'C', 'M')]
    if not con: sys.exit(f'{a.console}: no R / C / M intervals (a switch_trace timing capture?)')
    if a.host: ours = rows(a.host)
    elif a.log and a.frame is not None: ours = host_table(a.log, a.frame, tuple(map(int, a.passes.split('-'))) if a.passes else None)
    else: sys.exit('give --log and --frame, or --host')
    if a.host_tsv:
        with open(a.host_tsv, 'w') as o:
            cols = ['frame', 'idx', 'us', 'kind', 'w', 'h', 'colors', 'draws', 'dispatches', 'keys']
            o.write('\t'.join(cols) + '\n')
            for r in ours: o.write('\t'.join(str(r[c]) for c in cols) + '\n')
    labels = shader_labels(a.shader_map or (os.path.join(REPO, 'dm', 'games', a.game, 'shader_map.tsv') if a.game else None))
    per_frame = C.defaultdict(list)
    for r in con: per_frame[r['frame']].append(r)
    per = [attribute(rs) for rs in per_frame.values()]
    allk = set().union(*(p[0] for p in per)); allc = set().union(*(p[1] for p in per))
    ck = {k: st.median([p[0].get(k, 0.0) for p in per]) for k in allk}; cc = {c: st.median([p[1].get(c, 0.0) for p in per]) for c in allc}
    ctot = st.median([p[2] for p in per])
    ok, oc, otot = attribute(ours)
    print(f'console: {len(per_frame)} frames, median interval sum {ctot / 1000:.2f} ms; host: {otot / 1000:.2f} ms (sum of encoders)')
    lab = (lambda k: f' {labels[k]}' if k in labels else '') if labels else (lambda k: '')
    print('\n## By shader key (time split evenly over each interval\'s keys)\n')
    print('| key | console ms | host ms | console % | host % | host/console (ms) | host/console (share) |'); print('|---|---|---|---|---|---|---|')
    table = []
    for k in sorted(set(ck) | set(ok), key=lambda k: -(ok.get(k, 0) + ck.get(k, 0))):
        t, o = ck.get(k, 0.0), ok.get(k, 0.0); ts, os_ = t / ctot if ctot else 0, o / otot if otot else 0
        table.append((k, t, o, ts, os_, (o / t) if t else float('inf'), (os_ / ts) if ts else float('inf')))
    for k, t, o, ts, os_, r, rs_ in table[:a.top]:
        print(f'| {k}{lab(k)} | {t / 1000:.3f} | {o / 1000:.3f} | {100 * ts:.1f} | {100 * os_:.1f} | {r:.2f} | {rs_:.2f} |')
    print('\n## By kind and size class (relative to each side\'s largest colour target)\n')
    print('| kind | class | console ms | host ms | console % | host % | share ratio |'); print('|---|---|---|---|---|---|---|')
    for c in sorted(set(cc) | set(oc), key=lambda c: -(oc.get(c, 0) + cc.get(c, 0))):
        t, o = cc.get(c, 0.0), oc.get(c, 0.0); ts, os_ = t / ctot if ctot else 0, o / otot if otot else 0
        print(f'| {c[0]} | {c[1]} | {t / 1000:.3f} | {o / 1000:.3f} | {100 * ts:.1f} | {100 * os_:.1f} | {(os_ / ts) if ts else float("inf"):.2f} |')
    print('\n## Cheap on console, expensive here (by share of the frame; keys seen on both sides, at least 0.2 % on the host)\n')
    print('| key | host % | console % | share ratio | host ms |'); print('|---|---|---|---|---|')
    both = [x for x in table if x[1] > 0 and x[2] > 0 and x[4] >= 0.002]
    for k, t, o, ts, os_, r, rs_ in sorted(both, key=lambda x: (-x[6], -x[2]))[:a.top]:
        print(f'| {k}{lab(k)} | {100 * os_:.1f} | {100 * ts:.2f} | {rs_:.2f} | {o / 1000:.3f} |')
    only = [x for x in table if x[1] == 0 and x[2] > 0]
    if only: print(f'\nkeys with time on the host and none on the console: {len(only)} ({sum(x[2] for x in only) / 1000:.2f} ms); the console\'s keys may be missing ("?": a program '
                   'copied by value, or its code in a pool the CPU cannot read)')


if __name__ == '__main__':
    main()
