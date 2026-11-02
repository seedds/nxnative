#!/usr/bin/env python3
"""The sample profile at tier H (CTest `profile_weights`; docs/design/aot-recompiler.md 9.3 and 12, README.md "The sample profile at tier H"):
with a profile whose samples have positions inside the functions, `nxaot build --profile` weighs the lifted code of the functions at tiers H
and H+ before the tier's pipeline: branch weights between guest blocks from the flow profi infers from the blocks' samples, the function's
entry count, each call's count and a ProfileSummary (kind sample) per unit. The fixture is a profile of int_ops-gcc the host's sampler wrote
(tests/profile_weights.tsv, dm/host/aot_profile.c through dm/host/tests/t_aot_host.c: its header names the module, its build ID and image
SHA-256, which must still be the committed NRO's). Checks:

- the log's `aot: profile weights: <f> functions, <b> branches weighted, <e> entry counts, <c> call counts; <x> functions sampled at their
  entry only, not weighed; <z> with samples profi found no flow for, not weighed` holds what the profile and the lifted IR (--ir) hold: x the
  functions at H sampled at their entry only, f + z those sampled elsewhere too, an entry count on f of these (and on no other function), b
  branch weights other than the lowering's own (1 : 2^16, 1 : 2^20, 2^16 : 1), a ProfileSummary of kind sample in every unit with an entry
  count and in no other; the functions at tier C carry none of them;
- the pack is byte for byte the same with --workers 1 and 4, and in two runs;
- with the test switch NXAOT_PROFILE_WEIGHTS=0 the log says the weights are off and the IR has none of them, the lifted IR is the weighted one
  but for its metadata (and the weighed functions' section ".text"), and the pack is byte for byte the one of the same profile in the host
  sampler's form (every function's samples at its entry: not weighed), the pack the tiers alone give;
- with a cache: the profile again hits every unit (the same pack); a sample added outside every function, or a function's entry-only samples
  changed, hit every unit; a sample changed inside a weighed function misses its unit only, one for each of two functions in two units;
- the weighted pack, and the max mode's (its functions at H+ weighed, those at H not), print exactly corpus/expected/int_ops.out natively.

  test_profile_weights.py --nxaot <nxaot> --nxaot-host <nxaot_host> --work <dir>

Exit 0 when every check holds.
"""
import argparse
import bisect
import hashlib
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, '..', 'corpus')
FIXTURE = os.path.join(HERE, 'profile_weights.tsv')
NRO = os.path.join(CORPUS, 'nro', 'int_ops-gcc.nro')
EXPECTED = os.path.join(CORPUS, 'expected', 'int_ops.out')
WEIGHTS = re.compile(r'^aot: profile weights: (\d+) functions, (\d+) branches weighted, (\d+) entry counts, (\d+) call counts; (\d+) functions '
                     r'sampled at their entry only, not weighed; (\d+) with samples profi found no flow for, not weighed$', re.M)
CACHE = re.compile(r'^aot: cache \S+: (\d+) hits, (\d+) misses \(units; functions (\d+) hit, (\d+) missed\)', re.M)
LOWERING = {(1, 1 << 20), (1, 1 << 16), (1 << 16, 1)}  # the branch weights the lowering sets itself (polls, pending checks, inline caches)

failures = []


def check(ok, what):
    print(f'{"ok  " if ok else "FAIL"} {what}')
    if not ok:
        failures.append(what)
    return ok


def build(a, so, profile, *extra, env=None):
    e = dict(os.environ)
    e.pop('NXAOT_PROFILE_WEIGHTS', None)
    e.update(env or {})
    r = subprocess.run([a.nxaot, 'build', NRO, '--shared', so, '--profile', profile] + list(extra), capture_output=True, text=True, env=e)
    if r.returncode != 0:
        print(r.stdout + r.stderr)
        raise SystemExit(f'test_profile_weights.py: nxaot build {" ".join(extra)} failed (exit {r.returncode})')
    return r.stdout


def sha(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()


def read_profile(path):
    rows = {}
    for line in open(path):
        line = line.split('#', 1)[0].strip()
        if line:
            off, n = line.split()
            rows[int(off, 16)] = rows.get(int(off, 16), 0) + int(n)
    return rows


def write_profile(path, rows, header=''):
    with open(path, 'w') as f:
        f.write(header + ''.join(f'{off:#x} {n}\n' for off, n in sorted(rows.items(), key=lambda kv: (-kv[1], kv[0]))))


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
            out[int(e, 16)] = t
    return out


class Module:
    def __init__(self, name):
        self.name, self.defines, self.lines, self.md = name, [], [], {}


def modules(ir):
    """The units' modules of an --ir file: each its defines (name, the define line), its instruction lines and its metadata by number"""
    mods = []
    for line in ir.splitlines():
        m = re.match(r"^; ModuleID = '(.*)'$", line)
        if m:
            mods.append(Module(m.group(1)))
            continue
        if not mods:
            continue
        cur = mods[-1]
        m = re.match(r'^(!\d+) = (?:distinct )?(.*)$', line)
        if m:
            cur.md[m.group(1)] = m.group(2)
            continue
        if line.startswith('define '):
            n = re.search(r'@"([^"]+)"', line) or re.search(r'@([\w.$+]+)\(', line)
            cur.defines.append((n.group(1) if n else '?', line))
        cur.lines.append((cur.defines[-1][0] if cur.defines else None, line))
    return mods


def entry_of(name, prefix):
    m = re.fullmatch(re.escape(prefix) + r'\+0x([0-9A-Fa-f]+)', name)
    return int(m.group(1), 16) if m else None


def weights_in(mod, fn_filter):
    """The branch weights (lists of values) on br / switch lines, call counts on calls, of the functions fn_filter accepts"""
    branches, calls = [], 0
    for fn, line in mod.lines:
        if fn is None or not fn_filter(fn):
            continue
        m = re.search(r'!prof (!\d+)', line)
        if not m or line.startswith('define '):
            continue
        node = mod.md.get(m.group(1), '')
        vals = [int(v) for v in re.findall(r'i32 (\d+)', node)]
        if '"branch_weights"' not in node:
            continue
        s = line.lstrip()
        if s.startswith('br ') or s.startswith(']'):
            branches.append(tuple(vals))
        elif 'call ' in line:
            calls += 1
    return branches, calls


def normalise(ir):
    """The IR without its metadata: the nodes' definitions and numbers (added nodes renumber the others), the !prof attachments, and the
    weighed functions' section ".text\""""
    out = []
    for line in ir.splitlines():
        if line.startswith('!'):
            continue
        line = re.sub(r'!\d+', '!N', line)
        line = re.sub(r',? !prof !N', '', line)
        line = line.replace(' section ".text"', '')
        out.append(line)
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nxaot', required=True)
    ap.add_argument('--nxaot-host', required=True)
    ap.add_argument('--work', required=True)
    a = ap.parse_args()
    shutil.rmtree(a.work, ignore_errors=True)
    os.makedirs(a.work)
    w = lambda *p: os.path.join(a.work, *p)

    # The fixture names the committed NRO: its image SHA-256 (an NRO's image is the file), and its name and build ID as the host's sampler wrote them
    # (dm/host/tests/t_aot_host.c names an NRO it maps after its file and keeps no build ID: `int_ops-gcc`, `none`; a game's NRO has the NRO
    # header's build ID, at 0x40)
    nro = open(NRO, 'rb').read()
    head = open(FIXTURE).read()
    m = re.search(r'^# module (\S+), build ID (\S+), image SHA-256 ([0-9a-f]{64})$', head, re.M)
    check(m and m.group(1) in ('int_ops-gcc', 'int_ops-gcc.nro') and m.group(3) == hashlib.sha256(nro).hexdigest() and
          m.group(2) in ('none', nro[0x40:0x60].rstrip(b'\0').hex()), 'the fixture names int_ops-gcc.nro, its build ID and its image SHA-256')
    profile = read_profile(FIXTURE)
    fns = functions(a, a.work)
    starts = [e for e, _ in fns]

    def fn_of(off):
        i = bisect.bisect_right(starts, off) - 1
        return fns[i][0] if i >= 0 and off < fns[i][1] else None

    by_fn = {}
    for off, n in profile.items():
        f = fn_of(off)
        if f is not None and n:
            by_fn.setdefault(f, {})[off] = n
    positioned = {f for f, rows in by_fn.items() if any(off != f for off in rows)}
    entry_only = {f for f, rows in by_fn.items() if f not in positioned}

    # ---- The weighted pack, its lifted IR and its tiers ----
    os.makedirs(w('on'))
    log = build(a, w('on', 'x.so'), FIXTURE, '--workers', '4', '--ir', w('on.ll'), '--tiers-tsv', w('tiers.tsv'))
    t = tiers(w('tiers.tsv'))
    lw = WEIGHTS.search(log)
    check(lw is not None, 'the log has the profile weights line' + (f': {lw.group(0)}' if lw else ''))
    if not lw:
        return 1
    f_n, b_n, e_n, c_n, x_n, z_n = (int(g) for g in lw.groups())
    hot = {e for e, tier in t.items() if tier == 'H'}
    weighed = positioned & hot
    check(hot == set(by_fn), f'the sampled functions are at tier H ({len(hot)}), the others at C')
    check(x_n == len(entry_only & hot), f'{x_n} functions sampled at their entry only, as the profile has them ({len(entry_only & hot)})')
    check(b_n > 0 and c_n > 0 and e_n == f_n and f_n > 0 and f_n + z_n == len(weighed), f'{f_n} functions weighed and {z_n} without a flow '
          f'of {len(weighed)} sampled elsewhere than at their entry, {b_n} branches, {e_n} entry counts, {c_n} call counts')
    prefix = 'int_ops_gcc'
    mods = modules(open(w('on.ll')).read())
    counted, ours, lowering_only, c_extra, summaries, summary_ok = set(), 0, True, 0, 0, True
    for mod in mods:
        defined = {entry_of(n, prefix) for n, _ in mod.defines} - {None}
        has_count = False
        for name, line in mod.defines:
            e = entry_of(name, prefix)
            pm = re.search(r'!prof (!\d+)', line)
            if pm and 'function_entry_count' in mod.md.get(pm.group(1), ''):
                has_count = True
                counted.add(e)
        br, _ = weights_in(mod, lambda n: entry_of(n, prefix) in hot)
        ours += sum(1 for v in br if v not in LOWERING)
        cbr, ccalls = weights_in(mod, lambda n: entry_of(n.split('.')[0], prefix) in set(t) - hot)
        c_extra += sum(1 for v in cbr if v not in LOWERING) + ccalls
        summary = any('!"ProfileSummary"' in v for v in mod.md.values())
        summaries += summary
        kind = any('!"ProfileFormat", !"SampleProfile"' in v for v in mod.md.values())
        summary_ok = summary_ok and summary == has_count and (not summary or kind)
    check(counted <= weighed and len(counted) == e_n, f'entry counts on {len(counted)} functions, each at H with samples elsewhere than at its '
          f'entry (the log: {e_n})')
    check(ours == b_n, f'{ours} branch weights besides the lowering\'s own in the functions at H (the log: {b_n})')
    check(c_extra == 0, f'the functions at tier C: {c_extra} branch weights or call counts but the lowering\'s own')
    check(summary_ok and summaries > 0, f'a ProfileSummary of kind sample in the {summaries} units with an entry count, in no other')

    # ---- Determinism ----
    for name, extra in (('w1', ['--workers', '1']), ('again', ['--workers', '4'])):
        os.makedirs(w(name))
        build(a, w(name, 'x.so'), FIXTURE, *extra)
        check(sha(w(name, 'x.so')) == sha(w('on', 'x.so')), f'the pack with {" ".join(extra)} ({name}) is the same bytes')

    # ---- The test switch, and the host sampler's form of the same profile ----
    os.makedirs(w('off'))
    log_off = build(a, w('off', 'x.so'), FIXTURE, '--ir', w('off.ll'), env={'NXAOT_PROFILE_WEIGHTS': '0'})
    check('aot: profile weights: off (NXAOT_PROFILE_WEIGHTS=0)' in log_off, 'NXAOT_PROFILE_WEIGHTS=0: the log says the weights are off')
    ir_off = open(w('off.ll')).read()
    off_extra = sum(1 for mod in modules(ir_off) for v in weights_in(mod, lambda n: True)[0] if v not in LOWERING)
    check('function_entry_count' not in ir_off and '"ProfileSummary"' not in ir_off and off_extra == 0,
          'NXAOT_PROFILE_WEIGHTS=0: no entry count, summary or branch weight but the lowering\'s')
    check(normalise(ir_off) == normalise(open(w('on.ll')).read()), 'the weights add metadata only: the lifted IR is the same without it')
    entries = {f: sum(rows.values()) for f, rows in by_fn.items()}
    write_profile(w('entries.tsv'), entries, '# the fixture\'s samples at their functions\' entries, as the host\'s sampler writes them\n')
    os.makedirs(w('entries'))
    log_e = build(a, w('entries', 'x.so'), w('entries.tsv'))
    le = WEIGHTS.search(log_e)
    check(le and le.group(1) == '0' and int(le.group(5)) == len(hot), 'the host sampler\'s form: no function weighed, every one sampled at '
          'its entry only' + (f' ({le.group(0)})' if le else ''))
    check(sha(w('entries', 'x.so')) == sha(w('off', 'x.so')), 'its pack is the same bytes as NXAOT_PROFILE_WEIGHTS=0\'s: the tiers\' alone')
    check(sha(w('off', 'x.so')) != sha(w('on', 'x.so')), 'the weighted pack differs from it')

    # ---- The cache ----
    cache = w('cache')
    os.makedirs(w('c'))

    def cached(name, rows):
        path = w(f'{name}.tsv')
        write_profile(path, rows)
        out = build(a, w('c', 'x.so'), path, '--cache', cache)
        c = CACHE.search(out)
        return tuple(int(g) for g in c.groups()) if c else None, sha(w('c', 'x.so'))

    cold, cold_sha = cached('p0', profile)
    units = cold[0] + cold[1] if cold else 0
    check(cold is not None and cold[0] == 0 and units > 1, f'cold: {cold} (hits, misses, functions hit, missed)')
    warm, warm_sha = cached('p0', profile)
    check(warm is not None and warm[1] == 0 and warm[0] == units and warm_sha == cold_sha == sha(w('on', 'x.so')),
          f'the same profile again: {warm}, the same pack')
    outside = dict(profile)
    outside[max(l for _, l in fns) + 0x10000] = 7
    r, _ = cached('outside', outside)
    check(r is not None and r[1] == 0, f'a sample added outside every function: {r}')
    if entry_only & hot:
        f = min(entry_only & hot)
        moved = dict(profile)
        moved[f] += 5
        r, _ = cached('entry', moved)
        check(r is not None and r[1] == 0, f'the entry-only samples of {f:#x} changed: {r}')
    # Two weighed functions in different units (the units: the modules of the --ir file, each a unit's)
    unit_of = {}
    for i, mod in enumerate(mods):
        for name, _ in mod.defines:
            e = entry_of(name, prefix)
            if e is not None:
                unit_of.setdefault(e, i)
    picked = []
    for f in sorted(counted, key=lambda f: -sum(by_fn[f].values())):
        if all(unit_of.get(f) != unit_of.get(g) for g in picked):
            picked.append(f)
        if len(picked) == 2:
            break
    check(len(picked) == 2, f'two weighed functions in two units: {[hex(f) for f in picked]}')
    for f in picked:
        changed = dict(profile)
        off = max((o for o in by_fn[f] if o != f), key=lambda o: by_fn[f][o])
        changed[off] += 1
        r, _ = cached(f'fn{f:x}', changed)
        size = sum(1 for g, i in unit_of.items() if i == unit_of[f] and g in t)
        check(r is not None and r[1] == 1 and r[0] == units - 1 and r[3] == size,
              f'a sample of {f:#x} changed at {off:#x}: {r}: its unit missed ({size} functions), the others hit')

    # ---- The weighted packs run ----
    expected = open(EXPECTED).read()
    os.makedirs(w('max'))
    log_max = build(a, w('max', 'x.so'), FIXTURE, '--mode', 'max')
    lm = WEIGHTS.search(log_max)
    check(lm is not None and lm.group(1) == str(f_n), f'the max mode weighs the same functions (at H+): {lm.group(0) if lm else "no line"}')
    for name in ('on', 'max'):
        r = subprocess.run([a.nxaot_host, w(name, 'x.so'), NRO], capture_output=True, text=True)
        check(r.returncode == 0 and r.stdout == expected, f'the {name} pack runs natively and prints corpus/expected/int_ops.out (exit '
              f'{r.returncode})')

    print(f'profile weights: {len(fns)} functions, {len(hot)} at H, {f_n} weighed ({b_n} branches, {c_n} call counts), {x_n} at their entry '
          f'only; {len(failures)} failures')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
