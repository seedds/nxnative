#!/usr/bin/env python3
"""Check the files a mock run (mock_nvn.cpp) wrote against what the mock GPU and game must have produced.

usage: check.py <sd folder> <case>      cases: timing-run, timing-each, seq, dump-key, dump-all
       check.py --key <name>            print a mock shader's expected key (vs fs cs ts ms nvn1)
The keys are computed here from the mock's own control sections and code with hashlib, by hle_nvn.c keys_hash's rules, so this also checks the module's SHA-256 and
both key rules (NVN2, and NVN 1 with key_skip=0x778:0x1a)."""
import csv, hashlib, os, sys

KEY_SKIP = (0x778, 0x1a)


def code(seed, n):
    return bytes(((seed * 31 + i * 7) & 0xff) for i in range(n))


def nvn2_key(seed, ncode):
    ctl = bytearray(0x48); ctl[0:4] = (0x98761234).to_bytes(4, 'little'); ctl[4:8] = (0xc9).to_bytes(4, 'little'); ctl[8] = seed
    ctl[0x44:0x48] = ncode.to_bytes(4, 'little')   # +0x30 (0xAB in the mock) is zeroed by the rule
    return hashlib.sha256(bytes(ctl) + code(seed, ncode)).hexdigest()[:16]


def nvn1_key(seed, ncode):
    ctl = bytearray(((seed + i * 13) & 0xff) for i in range(0x880))
    ctl[0x20:0x24] = bytes(4); ctl[0x704:0x708] = ncode.to_bytes(4, 'little')
    off, n = KEY_SKIP; ctl[off:off + n] = bytes(n)
    return hashlib.sha256(bytes(ctl) + code(seed, ncode)).hexdigest()[:16]


K = dict(vs=nvn2_key(1, 0x800), fs=nvn2_key(2, 0x900), cs=nvn2_key(3, 0x700), ts=nvn2_key(4, 0x600), ms=nvn2_key(5, 0x500), nvn1=nvn1_key(6, 0x400))


def rows(path):
    with open(path) as f:
        return [r for r in csv.DictReader((l for l in f if not l.startswith('#')), delimiter='\t')]


def footer(path):
    return [l for l in open(path) if l.startswith('#')]


def keyset(s):
    return set(filter(None, (s or '').split(',')))


def check_timing(sd, each):
    vs, fs, cs, ts, ms, k1 = (K[x] for x in ('vs', 'fs', 'cs', 'ts', 'ms', 'nvn1'))
    # kind, us, draws, dispatches, groups, indirect, barriers, waits, signals, w, keys
    if each:
        want = [('C', 50.0, 0, 1, '', 1, 0, 0, 0, 0, {k1}), ('C', 50.0, 0, 1, '', 1, 0, 0, 0, 0, {k1}),
                ('R', 50.0, 5, 0, '', 0, 0, 0, 0, 1440, {vs, fs}),
                ('C', 8.0, 0, 1, '4x2x1', 0, 0, 0, 0, 1440, {cs}), ('C', 8.0, 0, 1, '8x1x1', 0, 0, 0, 0, 1440, {cs}), ('C', 8.0, 0, 1, '2x2x2', 0, 0, 0, 0, 1440, {cs}),
                ('R', 10.0, 1, 0, '', 0, 0, 0, 0, 1440, {vs, fs}),
                ('M', 20.0, 1, 0, '16', 0, 0, 0, 0, 1440, {ts, ms}), ('M', 20.0, 1, 0, '', 1, 0, 1, 0, 1440, {ts, ms}),
                ('R', 20.5, 2, 0, '', 0, 1, 0, 1, 720, {vs, fs})]
    else:
        want = [('C', 100.0, 0, 2, '', 1, 0, 0, 0, 0, {k1}),
                ('R', 50.0, 5, 0, '', 0, 0, 0, 0, 1440, {vs, fs}),
                ('C', 24.0, 0, 3, '24', 0, 0, 0, 0, 1440, {cs}),
                ('R', 10.0, 1, 0, '', 0, 0, 0, 0, 1440, {vs, fs}),
                ('M', 40.0, 2, 0, '', 1, 0, 1, 0, 1440, {ts, ms}),   # the mesh program (bound for fragment, task, mesh) replaced vs/fs's fragment stage
                ('R', 20.5, 2, 0, '', 0, 1, 0, 1, 720, {vs, fs})]
    path = os.path.join(sd, 'capture1.tsv'); rs = rows(path)
    frames = sorted({int(r['frame']) for r in rs})
    assert frames == list(range(frames[0], frames[0] + 40)), f'frames {frames[:3]}..{frames[-3:]} ({len(frames)})'
    bad = 0
    for f in frames:
        got = [r for r in rs if int(r['frame']) == f and r['kind'] in 'RCM']
        if len(got) != len(want): print(f'frame {f}: {len(got)} rows, want {len(want)}'); bad += 1; continue
        for r, w in zip(got, want):
            have = (r['kind'], float(r['us']), int(r['draws']), int(r['dispatches']), r['groups'], int(r['indirect']), int(r['barriers']), int(r['waits']), int(r['signals']), int(r['w']), keyset(r['keys']))
            if have != w: print(f'frame {f} idx {r["idx"]}: {have}\n   want {w}'); bad += 1
        if not any(r['kind'] == 'F' for r in rs if int(r['frame']) == f): print(f'frame {f}: no queue fence row'); bad += 1
    first = [r for r in rs if int(r['frame']) == frames[0] and r['kind'] in 'RCM']
    assert float(first[0]['start_us']) == 0.0, first[0]
    foot = footer(path)
    assert any('type1=3.0' in l and 'type16=1.0' in l for l in foot) and any('TIMESTAMP_TOP at 0x9000: 1.00 a frame' in l for l in foot), foot
    return bad, f'timing ({"each" if each else "run"}): {len(frames)} frames, {len(rs)} rows'


SEQ = ['submit', 'begin', 'bind', 'bind', 'dispatch', 'dispatch', 'end',
       'submit', 'begin', 'bind', 'bind', 'bind', 'bind', 'bind', 'targets', 'clear_color', 'bind', 'draw', 'bind', 'dispatch', 'dispatch', 'dispatch', 'draw', 'bind', 'mesh',
       'mesh', 'wait_event', 'targets', 'bind', 'barrier', 'render_cond', 'draw', 'render_enable', 'report', 'report', 'report', 'report', 'signal_event', 'end',
       'queue_fence', 'present']


def check_seq(sd):
    path = os.path.join(sd, 'seq1.tsv'); rs = rows(path)
    frames = sorted({int(r['frame']) for r in rs}); bad = 0
    for f in frames:
        got = [r for r in rs if int(r['frame']) == f]
        ops = [r['op'] for r in got]
        if ops != SEQ: print(f'frame {f}: ops\n  {ops}\n  want {SEQ}'); bad += 1; continue
        by = {}
        for r in got: by.setdefault(r['op'], []).append(r)
        checks = [
            (by['dispatch'][0]['args'], 'indirect'), (keyset(by['dispatch'][0]['keys']), {K['nvn1']}),
            ([r['args'] for r in by['dispatch'][2:]], ['4x2x1', '8x1x1', '2x2x2']), (keyset(by['dispatch'][2]['keys']), {K['cs']}),
            ([r['args'] for r in by['draw']], ['5', '1', '2']), (keyset(by['draw'][0]['keys']), {K['vs'], K['fs']}),
            ([r['args'] for r in by['mesh']], ['1', '1 indirect']), (keyset(by['mesh'][0]['keys']), {K['ts'], K['ms']}),
            ([r['args'] for r in by['targets']], ['1440x810 colors 1 cfmt 37 dfmt 52', '720x405 colors 1 cfmt 41 dfmt 0']),
            (by['barrier'][0]['args'], 'bits 0x1'), (by['wait_event'][0]['args'], 'event 0 mode 1 value 7'),
            (by['signal_event'][0]['args'], 'event 1 mode 0 location 1 flags 0 value 9'),
            (by['render_cond'][0]['args'], 'mode 1 addr 0x7000'), (by['render_enable'][0]['args'], '1'),
            ([r['args'] for r in by['report']], ['type 1 addr 0x5000', 'type 1 addr 0x5010', 'type 1 addr 0x5020', 'type 16 addr 0x9000']),
            (by['clear_color'][0]['args'], 'c0 mask 0xf'), (by['queue_fence'][0]['args'], 'sync 0'),
            ([r['cb'] for r in by['submit']], ['0', '1']),
        ]
        for i, (have, want) in enumerate(checks):
            if have != want: print(f'frame {f} check {i}: {have!r} want {want!r}'); bad += 1
    assert len(frames) == 40, len(frames)
    return bad, f'seq: {len(frames)} frames, {len(rs)} rows'


def check_dump(sd, case):
    path = os.path.join(sd, 'dump1.tsv'); rs = rows(path)
    want_n, tex, w, h, fmt, bpp = (5, 'tex_color.bin', 1440, 810, 37, 4) if case == 'dump-key' else (3, 'tex_half.bin', 720, 405, 41, 8)
    ref = open(os.path.join(sd, '..', tex), 'rb').read(); bad = 0
    if len(rs) != want_n: print(f'{len(rs)} dumps, want {want_n}'); bad += 1
    frames = []
    for r in rs:
        have = (r['kind'], int(r['w']), int(r['h']), int(r['nvn_format']), int(r['bpp']), int(r['bytes']))
        if have != ('color', w, h, fmt, bpp, w * h * bpp): print(f'dump {r["n"]}: {have}'); bad += 1
        if open(os.path.join(sd, r['file']), 'rb').read() != ref: print(f'dump {r["n"]}: bytes differ from the texture'); bad += 1
        frames.append(int(r['frame']))
    if case == 'dump-key' and frames != list(range(frames[0], frames[0] + want_n)): print(f'dump frames {frames}'); bad += 1
    return bad, f'{case}: {len(rs)} dumps, bytes match'


def main():
    if sys.argv[1] == '--key': print(K[sys.argv[2]]); return
    sd, case = sys.argv[1], sys.argv[2]
    bad, what = (check_timing(sd, case == 'timing-each') if case.startswith('timing') else check_seq(sd) if case == 'seq' else check_dump(sd, case))
    print(what + (': OK' if not bad else f': {bad} mismatches'))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
