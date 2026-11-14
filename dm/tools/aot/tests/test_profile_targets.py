#!/usr/bin/env python3
"""The hot indirect targets (CTest `profile_targets`; docs/design/aot-recompiler.md 4 and 12, README.md "Hot indirect targets"): with
`--profile <folder>/<module>.tsv`, nxaot reads <folder>/<module>.targets.tsv beside it when it is there (the host writes it at exit with DM_AOT_PROFILE:
dm/host/aot_targets.c), and an indirect call or branch site in a function at tier H or H+ whose dominant target (the only one, or at least 90 % of the
site's counts) is a function of the module (for a call, one that returns) is lowered as a compare of the computed address with the target's, a direct
call (tail call) of the target's body on a match and the inline cache otherwise. The fixture is fnptr-gcc's (tests/profile_targets.tsv, a sample at
each site of the functions meant for tier H; tests/profile_targets.targets.tsv beside it, the targets its sites reach, written by hand in the host's form
from the program's source, the same rows dm/host's t_aot_targets records from a run, and checked here against the NRO's words and discovery's functions).
Checks:

- the fixture: every site a blr or br of the NRO, every target an entry of discovery's functions, the header naming the NRO's image;
- the log's `aot: profile targets: <s> sites, <g> guarded, <m> with several targets, not guarded, <o> outside tier H; <x> whose target is no function of
  the module or a call's that does not return, <n> at no blr or br, <u> planned that no lifted code reaches` holds what the rule gives over the
  fixture, discovery's functions and the pack's tiers (--tiers-tsv), with g > 0 and u = 0;
- the lifted IR (--ir) of each guarded site holds its guard: a block guard_hit_<site>_ with the call (a call site) or musttail call (a tail site) of
  the target's body `@"fnptr_gcc+0x<target>"`, and the inline cache's slot of the site still there; no guard at any other site;
- the pack is byte for byte the same with --workers 1 and 4, and in two runs;
- NXAOT_PROFILE_TARGETS=0 logs the targets off, puts no guard in the IR, and gives the pack of the same profile without a targets file beside it (logged
  as none), which differs from the guarded pack;
- the 90 % rule: a site at 18 of 20 counts is guarded (fold's callback, shape_total's area, apply_op's tail call), one at 17 of 19 is not;
- with a cache: the same files hit every unit; targets changed at sites outside every guarded function (rt_init's at tier C, shape_total's still with
  several targets) hit every unit; a guarded target changed (with_cleanup's first call to hidden_target) misses its unit only, and so does a guarded
  site given a second target (main's call);
- the guarded packs run natively and print corpus/expected/fnptr.out: the balanced mode, the max mode, the ghccc convention, and the 90 % variant,
  whose guards miss on some calls (fold's other callbacks, apply_op's other operations, shape_total's other shapes) and take the inline cache.

  test_profile_targets.py --nxaot <nxaot> --nxaot-host <nxaot_host> --work <dir>

Exit 0 when every check holds.
"""
import argparse
import bisect
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, '..', 'corpus')
PROFILE = os.path.join(HERE, 'profile_targets.tsv')
TARGETS = os.path.join(HERE, 'profile_targets.targets.tsv')
NRO = os.path.join(CORPUS, 'nro', 'fnptr-gcc.nro')
SYMS = os.path.join(CORPUS, 'nro', 'fnptr-gcc.syms.tsv')
EXPECTED = os.path.join(CORPUS, 'expected', 'fnptr.out')
PREFIX = 'fnptr_gcc'
TARGETS_LINE = re.compile(r'^aot: profile targets: (\d+) sites, (\d+) guarded, (\d+) with several targets, not guarded, (\d+) outside tier H; (\d+) whose '
                          r'target is no function of the module or a call\'s that does not return, (\d+) at no blr or br, (\d+) planned that no lifted code '
                          r'reaches', re.M)
CACHE = re.compile(r'^aot: cache \S+: (\d+) hits, (\d+) misses \(units; functions (\d+) hit, (\d+) missed\)', re.M)

failures = []


def check(ok, what):
    print(f'{"ok  " if ok else "FAIL"} {what}')
    if not ok:
        failures.append(what)
    return ok


def build(a, so, profile, *extra, env=None):
    e = dict(os.environ)
    e.pop('NXAOT_PROFILE_TARGETS', None)
    e.pop('NXAOT_PROFILE_WEIGHTS', None)
    e.update(env or {})
    r = subprocess.run([a.nxaot, 'build', NRO, '--shared', so, '--profile', profile] + list(extra), capture_output=True, text=True, env=e)
    if r.returncode != 0:
        print(r.stdout + r.stderr)
        raise SystemExit(f'test_profile_targets.py: nxaot build {" ".join(extra)} failed (exit {r.returncode})')
    return r.stdout


def sha(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()


def read_targets(path):
    """{site: {target: count}} of a targets file"""
    rows = {}
    for line in open(path):
        line = line.split('#', 1)[0].strip()
        if line:
            s, t, n = line.split()
            rows.setdefault(int(s, 16), {})
            rows[int(s, 16)][int(t, 16)] = rows[int(s, 16)].get(int(t, 16), 0) + int(n)
    return rows


def write_targets(path, rows):
    with open(path, 'w') as f:
        f.write('# a variant of the fixture (test_profile_targets.py)\n')
        for s in sorted(rows):
            for t in sorted(rows[s]):
                f.write(f'{s:#x} {t:#x} {rows[s][t]}\n')


def functions(a, work):
    """Discovery's functions: (entry, limit), in address order"""
    tsv = os.path.join(work, 'discover.tsv')
    subprocess.run([a.nxaot, 'discover', NRO, '--tsv', tsv], capture_output=True, check=True)
    out = []
    for line in open(tsv):
        f = line.rstrip('\n').split('\t')
        if f[0] == 'fn':
            out.append((int(f[1], 16), int(f[3], 16)))
    return sorted(out)


def tiers(path):
    out = {}
    for line in open(path):
        if not line.startswith('#'):
            e, t = line.split('\t')[:2]
            out[int(e, 16)] = t.strip()
    return out


def symbols():
    """corpus/nro/fnptr-gcc.syms.tsv's functions: name -> entry"""
    out = {}
    for line in open(SYMS):
        f = line.rstrip('\n').split('\t')
        if len(f) >= 5 and f[2] == 'func' and int(f[1], 16):
            out[f[4]] = int(f[0], 16)
    return out


def word(img, off):
    return struct.unpack_from('<I', img, off)[0] if off + 4 <= len(img) and off % 4 == 0 else None


def site_kind(img, off):
    """'call' for a blr, 'tail' for a br other than br x30, None for anything else"""
    w = word(img, off)
    if w is None:
        return None
    if w & 0xFFFFFC1F == 0xD63F0000:
        return 'call'
    if w & 0xFFFFFC1F == 0xD61F0000 and (w >> 5) & 31 != 30:
        return 'tail'
    return None


def expected_guards(rows, fns, tier, img):
    """The rule (README.md, "Hot indirect targets") over the rows: {site: target} guarded, and the log's counts. A call's target returning is not known
    here: fnptr's targets all return, so the count of those that do not is 0"""
    starts = [e for e, _ in fns]
    entries = set(starts)
    guards, counts = {}, dict(sites=0, several=0, outside=0, nofn=0, nobr=0)
    for s, ts in sorted(rows.items()):
        counts['sites'] += 1
        i = bisect.bisect_right(starts, s) - 1
        if i < 0 or s >= fns[i][1] or tier.get(fns[i][0]) not in ('H', 'H+'):
            counts['outside'] += 1
            continue
        total = sum(ts.values())
        to, best = min(ts.items(), key=lambda kv: (-kv[1], kv[0]))
        if best * 10 < total * 9:
            counts['several'] += 1
            continue
        if site_kind(img, s) is None:
            counts['nobr'] += 1
            continue
        if to not in entries:
            counts['nofn'] += 1
            continue
        guards[s] = to
    return guards, counts


def ir_functions(ir):
    """The defined functions of an --ir file: name -> its lines (from `define` to `}`)"""
    out, cur = {}, None
    for line in ir.splitlines():
        if line.startswith('define '):
            m = re.search(r'@"([^"]+)"', line) or re.search(r'@([\w.$+]+)\(', line)
            cur = m.group(1) if m else '?'
            out[cur] = [line]
        elif cur is not None:
            out[cur].append(line)
            if line == '}':
                cur = None
    return out


def guard_blocks(lines):
    """A function's guard_hit blocks: {site: [the block's lines]} (every copy of a site's block, LLVM numbering the later ones)"""
    out, cur = {}, None
    for line in lines:
        m = re.match(r'^([\w.$+-]+):', line)
        if m:
            g = re.fullmatch(r'guard_hit_([0-9A-Fa-f]+)_\d*', m.group(1))
            cur = int(g.group(1), 16) if g else None
            if cur is not None:
                out.setdefault(cur, [])
            continue
        if cur is not None:
            out[cur].append(line)
    return out


def body(entry):
    return f'{PREFIX}+0x{entry:X}'


def fn_of(fns, off):
    starts = [e for e, _ in fns]
    i = bisect.bisect_right(starts, off) - 1
    return fns[i][0] if i >= 0 and off < fns[i][1] else None


def check_ir(ir_text, guards, rows, fns, img, what):
    """Each guarded site's guard_hit block calls its target's body (musttail for a tail site), its inline cache's slot is still there, and no other
    site has a guard"""
    fs = ir_functions(ir_text)
    seen, bad = set(), []
    for name, lines in fs.items():
        for s, blk in guard_blocks(lines).items():
            seen.add(s)
            if s not in guards:
                bad.append(f'{s:#x} has a guard in {name}')
                continue
            t = guards[s]
            call = [ln for ln in blk if f'@"{body(t)}"(' in ln and ' call ' in ln]
            tail = site_kind(img, s) == 'tail'
            if not call or (tail and not any('musttail call' in ln for ln in call)):
                bad.append(f'{s:#x}: its guard_hit block in {name} has no {"musttail " if tail else ""}call of {body(t)}')
            if not any(f'nxaot.ic.' in ln and re.search(rf'nxaot\.ic\.[0-9A-F]+\.{s:X}\b', ln) for ln in lines):
                bad.append(f'{s:#x}: no inline-cache slot nxaot.ic.*.{s:X} left in {name}')
    missing = set(guards) - seen
    if missing:
        bad.append('no guard at ' + ', '.join(f'{s:#x}' for s in sorted(missing)))
    check(not bad, f'{what}: the IR has the guards of {len(guards)} sites ({", ".join(f"{s:#x}->{t:#x}" for s, t in sorted(guards.items()))}) and no other'
          + (': ' + '; '.join(bad) if bad else ''))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nxaot', required=True)
    ap.add_argument('--nxaot-host', required=True)
    ap.add_argument('--work', required=True)
    a = ap.parse_args()
    shutil.rmtree(a.work, ignore_errors=True)
    os.makedirs(a.work)
    w = lambda *p: os.path.join(a.work, *p)

    img = open(NRO, 'rb').read()
    head = open(TARGETS).read()
    m = re.search(r'^# module (\S+), build ID (\S+), image SHA-256 ([0-9a-f]{64})$', head, re.M)
    check(m and m.group(1) == 'fnptr-gcc' and m.group(3) == hashlib.sha256(img).hexdigest(), 'the fixture names fnptr-gcc.nro and its image SHA-256')
    rows = read_targets(TARGETS)
    fns = functions(a, a.work)
    entries = {e for e, _ in fns}
    bad = [f'{s:#x}' for s in rows if site_kind(img, s) is None] + [f'{s:#x}->{t:#x}' for s, ts in rows.items() for t in ts if t not in entries]
    check(not bad, f'the fixture\'s {len(rows)} sites are blr or br words of the NRO and its targets entries of discovery\'s {len(fns)} functions'
          + (': not ' + ', '.join(bad) if bad else ''))
    syms = symbols()
    named = {syms.get('with_cleanup'), syms.get('main'), syms.get('rt_exit')}
    check(all(fn_of(fns, s) in named for s, ts in rows.items() if len(ts) == 1), 'the sites of one target are with_cleanup\'s, main\'s and rt_exit\'s')

    # ---- The guarded pack, its log, its IR, its tiers ----
    os.makedirs(w('on'))
    log = build(a, w('on', 'x.so'), PROFILE, '--workers', '4', '--ir', w('on.ll'), '--tiers-tsv', w('tiers.tsv'))
    t = tiers(w('tiers.tsv'))
    guards, c = expected_guards(rows, fns, t, img)
    lt = TARGETS_LINE.search(log)
    check(lt is not None, 'the log has the profile targets line' + (f': {lt.group(0)}' if lt else ''))
    if not lt:
        print(log)
        return 1
    s_n, g_n, m_n, o_n, x_n, n_n, u_n = (int(v) for v in lt.groups())
    check((s_n, g_n, m_n, o_n, x_n, n_n, u_n) == (c['sites'], len(guards), c['several'], c['outside'], c['nofn'], c['nobr'], 0) and g_n > 0,
          f'the line holds the rule\'s counts: {c["sites"]} sites, {len(guards)} guarded, {c["several"]} with several targets, {c["outside"]} outside tier H, '
          f'{c["nofn"]} no function, {c["nobr"]} no branch, none unreached')
    check(o_n >= 1 and t.get(syms.get('rt_init')) == 'C', f'rt_init is at tier C and its site outside tier H ({o_n} outside)')
    check_ir(open(w('on.ll')).read(), guards, rows, fns, img, 'the guarded pack')

    # ---- Determinism ----
    for name, extra in (('w1', ['--workers', '1']), ('again', ['--workers', '4'])):
        os.makedirs(w(name))
        build(a, w(name, 'x.so'), PROFILE, *extra)
        check(sha(w(name, 'x.so')) == sha(w('on', 'x.so')), f'the pack with {" ".join(extra)} ({name}) is the same bytes')

    # ---- The test switch, and the same profile without a targets file ----
    os.makedirs(w('off'))
    log_off = build(a, w('off', 'x.so'), PROFILE, '--ir', w('off.ll'), env={'NXAOT_PROFILE_TARGETS': '0'})
    check('aot: profile targets: off (NXAOT_PROFILE_TARGETS=0)' in log_off, 'NXAOT_PROFILE_TARGETS=0: the log says the targets are off')
    check('guard_hit_' not in open(w('off.ll')).read(), 'NXAOT_PROFILE_TARGETS=0: no guard in the IR')
    os.makedirs(w('alone'))
    shutil.copy(PROFILE, w('alone', 'fnptr-gcc.tsv'))
    os.makedirs(w('none'))
    log_none = build(a, w('none', 'x.so'), w('alone', 'fnptr-gcc.tsv'))
    check(f'aot: profile targets: none (no {w("alone", "fnptr-gcc.targets.tsv")})' in log_none, 'the profile alone: the log says there is no targets file')
    check(sha(w('none', 'x.so')) == sha(w('off', 'x.so')), 'NXAOT_PROFILE_TARGETS=0 gives the pack of the profile alone, byte for byte')
    check(sha(w('off', 'x.so')) != sha(w('on', 'x.so')), 'the guarded pack differs from it')

    # ---- The 90 % rule ----
    variants = {}
    skew = {k: dict(v) for k, v in rows.items()}
    cb_sum, cb_max, cb_xor = syms['cb_sum'], syms['cb_max'], syms['cb_xor_rot']
    fold_site = next(s for s, ts in rows.items() if set(ts) == {cb_sum, cb_max, cb_xor})
    area = next(s for s, ts in rows.items() if syms['rect_area'] in ts)
    tail_site = next(s for s, ts in rows.items() if syms['op_sq'] in ts)
    skew[fold_site] = {cb_sum: 18, cb_max: 1, cb_xor: 1}
    skew[area] = {syms['rect_area']: 18, syms['tri_area']: 1, syms['sq_area']: 1}
    skew[tail_site] = {syms['op_sq']: 18, syms['op_inc']: 1, syms['op_pop']: 1}
    under = {k: dict(v) for k, v in rows.items()}
    under[fold_site] = {cb_sum: 17, cb_max: 1, cb_xor: 1}
    for name, vr in (('skew', skew), ('under', under)):
        os.makedirs(w(name))
        shutil.copy(PROFILE, w(name, 'fnptr-gcc.tsv'))
        write_targets(w(name, 'fnptr-gcc.targets.tsv'), vr)
        os.makedirs(w(name, 'p'))
        lg = build(a, w(name, 'p', 'x.so'), w(name, 'fnptr-gcc.tsv'), '--ir', w(f'{name}.ll'))
        gv, cv = expected_guards(vr, fns, t, img)
        lv = TARGETS_LINE.search(lg)
        variants[name] = (gv, lv)
        check(lv is not None and int(lv.group(2)) == len(gv), f'{name}: {lv.group(0) if lv else "no targets line"} ({len(gv)} guarded by the rule)')
        check_ir(open(w(f'{name}.ll')).read(), gv, vr, fns, img, name)
    check(set(variants['skew'][0]) == set(guards) | {fold_site, area, tail_site} and variants['skew'][0][tail_site] == syms['op_sq'],
          f'18 of 20 guards fold\'s callback ({fold_site:#x}), shape_total\'s area ({area:#x}) and apply_op\'s tail call ({tail_site:#x})')
    check(fold_site not in variants['under'][0] and set(variants['under'][0]) == set(guards), f'17 of 19 does not guard fold\'s callback ({fold_site:#x})')

    # ---- The cache ----
    cache = w('cache')
    os.makedirs(w('c'))
    shutil.copy(PROFILE, w('c', 'fnptr-gcc.tsv'))

    def cached(vr):
        write_targets(w('c', 'fnptr-gcc.targets.tsv'), vr)
        out = build(a, w('c', 'x.so'), w('c', 'fnptr-gcc.tsv'), '--cache', cache)
        cm = CACHE.search(out)
        return tuple(int(g) for g in cm.groups()) if cm else None

    cold = cached(rows)
    units = cold[0] + cold[1] if cold else 0
    check(cold is not None and cold[0] == 0 and units > 1, f'cold: {cold} (hits, misses, functions hit, missed)')
    warm = cached(rows)
    check(warm is not None and warm == (units, 0, cold[2] + cold[3], 0) and sha(w('c', 'x.so')) == sha(w('on', 'x.so')),
          f'the same files again: {warm}, the guarded pack\'s bytes')
    rt_init_site = next(s for s, ts in rows.items() if fn_of(fns, s) == syms['rt_init'])
    outside = {k: dict(v) for k, v in rows.items()}
    outside[rt_init_site] = {syms['ctor_first']: 5}
    outside[area] = {k2: v2 + 3 for k2, v2 in rows[area].items()}
    r = cached(outside)
    check(r is not None and r[1] == 0, f'targets changed at rt_init\'s site (tier C) and shape_total\'s (several still): {r}')
    wc = sorted(s for s in rows if fn_of(fns, s) == syms['with_cleanup'])
    inside = {k: dict(v) for k, v in rows.items()}
    inside[wc[0]] = {syms['hidden_target']: 1}
    r = cached(inside)
    check(r is not None and r[1] == 1 and r[0] == units - 1, f'with_cleanup\'s first guarded target changed ({wc[0]:#x} -> hidden_target): {r}: its unit missed')
    main_site = next(s for s in rows if fn_of(fns, s) == syms['main'])
    two = {k: dict(v) for k, v in rows.items()}
    two[main_site][syms['work']] = 1
    r = cached(two)
    check(r is not None and r[1] == 1 and r[0] == units - 1, f'main\'s guarded site given a second target ({main_site:#x}): {r}: its unit missed')

    # ---- The guarded packs run ----
    expected = open(EXPECTED).read()
    os.makedirs(w('max'))
    log_max = build(a, w('max', 'x.so'), PROFILE, '--mode', 'max')
    lm = TARGETS_LINE.search(log_max)
    check(lm is not None and int(lm.group(2)) == len(guards) and int(lm.group(4)) == 0,
          f'the max mode (every function at H or H+) guards the same sites: {lm.group(0) if lm else "no line"}')
    os.makedirs(w('ghc'))
    log_ghc = build(a, w('ghc', 'x.so'), PROFILE, '--cc', 'ghc')
    lg = TARGETS_LINE.search(log_ghc)
    check(lg is not None and int(lg.group(2)) == len(guards), f'the ghccc convention guards the same sites: {lg.group(0) if lg else "no line"}')
    for name, so in (('on', w('on', 'x.so')), ('max', w('max', 'x.so')), ('ghc', w('ghc', 'x.so')), ('skew', w('skew', 'p', 'x.so'))):
        r = subprocess.run([a.nxaot_host, so, NRO], capture_output=True, text=True)
        check(r.returncode == 0 and r.stdout == expected, f'the {name} pack runs natively and prints corpus/expected/fnptr.out (exit {r.returncode})')

    print(f'profile targets: {len(fns)} functions, {s_n} sites, {g_n} guarded ({", ".join(f"{s:#x}" for s in sorted(guards))}), {m_n} with several targets, '
          f'{o_n} outside tier H; {len(failures)} failures')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
