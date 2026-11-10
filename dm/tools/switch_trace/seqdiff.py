#!/usr/bin/env python3
"""Diff the order of GPU work on the console (a switch_trace sequence log) against the host's for the same scene (DM_NVN_PASSDUMP lines).

usage: seqdiff.py <console seqN.tsv> --log <host game.log> --frame F [--passes LO-HI] [--console-frame N] [--context 3] [--all]

Both sides become one stream of tokens per frame, in submission / encode order:
  T <class> c<colours> [d]   render targets that the game draws or clears into (console: SetRenderTargets followed by a draw, mesh draw or clear; host: a pass's P lines).
                             <class> is the size relative to that side's largest colour target (full, half, quarter, eighth, small, tiny), so resolutions line up
  C <key>                    a dispatch, by its shader key (host: X lines)
  B <bits>                   a barrier the game asked for (host: I lines)
  S                          an event signal (host: Y signal_event)
  F                          a command-buffer fence (host: Y fence_sync)
  RE <0|1>, RC               render enable, conditional render (host: E lines)
Console event waits (W) have no host line; they are counted, and shown with --all.
The host's own restructuring shows up as differences: passes it split (M lines) or merged (F lines), dispatches it moved ahead of a pass (H lines). The host log needs
DM_NVN_PASSDUMP=F (DM_NVN_PASSDUMP_ALL=1 is not needed). --passes limits the host to passes LO-HI of that frame."""
import argparse, collections as C, csv, difflib, re, sys

P_LINE = re.compile(r'\[pass\] F(\d+) P (\d+) (?:(c\d+|d|s) ser=\d+ fmt=\d+ (\d+)x(\d+)|attachment-less (\d+)x(\d+))')
X_LINE = re.compile(r'\[pass\] F(\d+) X (\d+) dispatch ([0-9a-f]{16})')
I_LINE = re.compile(r'\[pass\] F(\d+) I (\d+) barrier bits (0x[0-9a-f]+|0)')
E_LINE = re.compile(r'\[pass\] F(\d+) E (\d+) (render enable (\d)|conditional render)')
Y_LINE = re.compile(r'\[pass\] F(\d+) Y (signal_event|fence_sync)')


def size_class(w, h, ref):
    if w * h <= 64 * 64: return 'tiny'
    s = ((w * h) / ref) ** 0.5 if ref else 1
    return 'full' if s >= 0.7 else 'half' if s >= 0.35 else 'quarter' if s >= 0.18 else 'eighth' if s >= 0.09 else 'small'


def console_tokens(path, frame=None):
    with open(path) as f:
        rs = [r for r in csv.DictReader((l for l in f if not l.startswith('#')), delimiter='\t')]
    frames = sorted({int(r['frame']) for r in rs})
    if not frames: sys.exit(f'{path}: no rows')
    frame = frames[0] if frame is None else frame
    rs = [r for r in rs if int(r['frame']) == frame]
    ref = 0
    for r in rs:
        m = re.match(r'(\d+)x(\d+) colors (\d+)', r['args']) if r['op'] == 'targets' else None
        if m and int(m[3]): ref = max(ref, int(m[1]) * int(m[2]))
    toks, waits, pending = [], 0, None   # pending: targets not yet drawn into
    for r in rs:
        op, args = r['op'], r['args']; where = f"console {r['idx']} {op} {args}".strip()
        if op == 'targets':
            m = re.match(r'(\d+)x(\d+) colors (\d+) cfmt -?\d+ dfmt (-?\d+)', args)
            w, h, c, d = int(m[1]), int(m[2]), int(m[3]), int(m[4])
            pending = (len(toks), (f'T {size_class(w, h, ref) if c else "depth" if d else "none"} c{c}{" d" if d else ""}', where))   # placed where it was set, once drawn into
        elif op in ('draw', 'mesh', 'clear_color', 'clear_ds'):
            if pending: toks.insert(pending[0], pending[1]); pending = None
        elif op == 'dispatch': toks.append((f'C {(r["keys"] or "?").split(",")[0][:16]}', where))
        elif op == 'barrier': toks.append((f'B {args.split()[-1]}', where))
        elif op == 'signal_event': toks.append(('S', where))
        elif op == 'fence_sync': toks.append(('F', where))
        elif op == 'render_enable': toks.append((f'RE {args}', where))
        elif op == 'render_cond': toks.append(('RC', where))
        elif op == 'wait_event' or op == 'wait_sync': waits += 1; toks.append(('W', where))
    return frame, toks, waits


def host_tokens(log, frame, passes=None):
    passes_seen, toks = {}, []
    with open(log, errors='replace') as f:
        for ln in f:
            if f'[pass] F{frame} ' not in ln: continue
            m = P_LINE.search(ln)
            if m:
                p = int(m[2])
                if passes and not passes[0] <= p <= passes[1]: continue
                if p not in passes_seen:
                    passes_seen[p] = dict(c=0, d=False, w=0, h=0, at=len(toks)); toks.append(None)   # filled in once all its P lines are read
                e = passes_seen[p]
                if m[3] and m[3].startswith('c'): e['c'] += 1; e['w'], e['h'] = (int(m[4]), int(m[5])) if not e['w'] or m[3] == 'c0' else (e['w'], e['h'])
                elif m[3] in ('d', 's'): e['d'] = True; e['w'], e['h'] = (e['w'], e['h']) if e['w'] else (int(m[4]), int(m[5]))
                else: e['w'], e['h'] = int(m[6]), int(m[7])
                continue
            for rx, kind in ((X_LINE, 'C'), (I_LINE, 'B'), (E_LINE, 'E'), (Y_LINE, 'Y')):
                m = rx.search(ln)
                if not m: continue
                if kind != 'Y' and passes and not passes[0] - 1 <= int(m[2]) <= passes[1]: break
                where = 'host ' + ln.split('] ', 1)[1].strip()[:100]
                if kind == 'C': toks.append((f'C {m[3]}', where))
                elif kind == 'B': toks.append((f'B {int(m[3], 16):#x}' if m[3] != '0' else 'B 0x0', where))
                elif kind == 'E': toks.append((f'RE {m[4]}' if m[4] is not None else 'RC', where))
                else: toks.append(('S' if m[2] == 'signal_event' else 'F', where))
                break
    if not passes_seen and not toks: sys.exit(f'{log}: no [pass] lines for frame {frame} (DM_NVN_PASSDUMP={frame})')
    ref = max((e['w'] * e['h'] for e in passes_seen.values() if e['c']), default=0)
    for p, e in passes_seen.items():
        cls = size_class(e['w'], e['h'], ref) if e['c'] else 'depth' if e['d'] else 'none'
        toks[e['at']] = (f'T {cls} c{e["c"]}{" d" if e["d"] else ""}', f'host pass {p} {e["w"]}x{e["h"]}')
    return [t for t in toks if t]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('console'); ap.add_argument('--log', required=True); ap.add_argument('--frame', type=int, required=True); ap.add_argument('--passes')
    ap.add_argument('--console-frame', type=int); ap.add_argument('--context', type=int, default=3); ap.add_argument('--all', action='store_true', help='keep console waits in the diff')
    a = ap.parse_args(argv)
    cf, ctoks, waits = console_tokens(a.console, a.console_frame)
    if not a.all: ctoks = [t for t in ctoks if t[0] != 'W']
    htoks = host_tokens(a.log, a.frame, tuple(map(int, a.passes.split('-'))) if a.passes else None)
    ca, ha = [t[0] for t in ctoks], [t[0] for t in htoks]
    sm = difflib.SequenceMatcher(None, ca, ha, autojunk=False)
    kind = lambda t: t.split()[0]
    cnt = C.Counter(kind(t) for t in ca); hnt = C.Counter(kind(t) for t in ha); same = C.Counter()
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == 'equal':
            for t in ca[i1:i2]: same[kind(t)] += 1
    print(f'console frame {cf}: {len(ca)} tokens ({waits} waits {"kept" if a.all else "left out"}); host frame {a.frame}: {len(ha)} tokens; in the same order: {sum(same.values())}')
    print('\n| token | console | host | in order |'); print('|---|---|---|---|')
    for k in sorted(set(cnt) | set(hnt)): print(f'| {k} | {cnt[k]} | {hnt[k]} | {same[k]} |')
    print('\n## Differences (- console only, + host only)\n')
    for g in sm.get_grouped_opcodes(a.context):
        print(f'@@ console {g[0][1]}-{g[-1][2]}, host {g[0][3]}-{g[-1][4]} @@')
        for tag, i1, i2, j1, j2 in g:
            if tag == 'equal':
                for t in ctoks[i1:i2]: print(f'  {t[0]:<26} {t[1]}')
                continue
            for t in ctoks[i1:i2]: print(f'- {t[0]:<26} {t[1]}')
            for t in htoks[j1:j2]: print(f'+ {t[0]:<26} {t[1]}')


if __name__ == '__main__':
    main()
