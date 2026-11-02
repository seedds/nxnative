#!/usr/bin/env python3
"""Gap code (CTest `gaps`; docs/design/aot-recompiler.md 1.3 "Coverage" and 9.3, README.md "Gap code at tier C"): a game-shaped
module whose functions are a quarter without an FDE (`gen_bench.py --direct --functions 4000 --no-fde-percent 25`: 1,000 of the 4,000
statement functions, chosen by the seed, have no FDE and no driver calls them; their true ranges in main.nofde.tsv), then:

- the generator: .eh_frame_hdr holds an FDE for every function but those 1,000 (4,048 functions with the drivers, 3,048 FDEs), sorted;
- discovery (`nxaot discover --tsv`): every function without an FDE found at its entry with its exact bounds (none split into pieces, none
  merged into its neighbour, none missed), every function with an FDE too. By provenance: 138 by the statements' calls (`call`), 97 by the
  table of function pointers (`reloc`), 25 by both, 719 by the gap sweep alone (`gap`: code nothing names, the dead functions of this module),
  21 by the gap sweep and a call (`call,gap`: their only callers are gap code, which the sweep found in the same round); the summary line
  counts them as `gap 719/740` (primary / any). The numbers are this module's (the generator and discovery are deterministic): before
  discovery took an FDE-less function's walked span as its own range (README.md, "Gap code at tier C"), the cases of a switch it did not bound
  became entries of their own and cut such a function in pieces;
- `nxaot build --tier W` (the default: gap code takes its mode's tier): the log says `aot: gap code: 719 functions, 34372 instructions` (no
  "at tier C") and `aot: tiers C 0 W 4049 H 0 H+ 0`; `--tiers-tsv` lists every function at W and marks as gap code exactly discovery's
  `gap` functions;
- `nxaot build --tier W --gap-tier C`: the log says `aot: gap code: 719 functions, 34372 instructions at tier C` and `aot: tiers C 719 W
  3330 H 0 H+ 0`; `--tiers-tsv` lists every gap function at C and every other function at W;
- in both, the pack is the same bytes with --workers 1 and --workers 4, and a warm repack (the analysis and every unit from the cache, the
  gap flag with the analysis) gives the same tiers and the same pack; the two packs differ;
- with `--gap-tier C` and `--profile` sampling one gap function (balanced mode): that function at H (the profile wins), the other 718 at C
  with every other function (C 4048 W 0 H 1 H+ 0), and the log counts the sampled one apart;
- the max mode (`--mode max --gap-tier C`): every function at H+, gap code included (the option is the balanced mode's), and the log's gap
  line has no "at tier C".

  test_gaps.py --nxaot <nxaot> --work <dir>

Exit 0 when every check holds.
"""
import argparse
import bisect
import collections
import os
import re
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(HERE, 'gen_bench.py')
FUNCTIONS, PERCENT, SEED = 4000, 25, 1
# What this module gives (measured; README.md, "Gap code at tier C")
WANT_PROV = {'call': 138, 'reloc': 97, 'reloc,call': 25, 'gap': 719, 'call,gap': 21}  # the FDE-less functions by their provenance
WANT_GAP_LINE = 'aot: gap code: 719 functions, 34372 instructions'      # the default; with --gap-tier C, ' at tier C' after it
WANT_TIERS = {'W': 'aot: tiers C 0 W 4049 H 0 H+ 0 (functions)', 'C': 'aot: tiers C 719 W 3330 H 0 H+ 0 (functions)'}  # by gap code's tier
WANT_SUMMARY_GAP = '719/740'


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f'test_gaps.py: {" ".join(cmd)} failed (exit {r.returncode}):\n{r.stdout}{r.stderr}')
    return r.stdout


def eh_frame_hdr(image):
    """The functions of .eh_frame_hdr's sorted table, (start, end) in its order (MOD0's eh_frame_hdr field; gen_bench.py's layout: each FDE's pc
    range at +12)"""
    mod0 = struct.unpack_from('<I', image, 4)[0]
    hdr = mod0 + struct.unpack_from('<i', image, mod0 + 16)[0]
    out = []
    for k in range(struct.unpack_from('<I', image, hdr + 8)[0]):
        start, fde = (hdr + v for v in struct.unpack_from('<ii', image, hdr + 12 + 8 * k))
        out.append((start, start + struct.unpack_from('<i', image, fde + 12)[0]))
    return out


def bounds(fns, entries, ranges):
    """Of the true ranges [start, end): how many discovery found at their start with their exact size and no entry inside, the entries inside
    them (pieces), those merged into the function before, those missed"""
    exact = pieces = merged = missed = 0
    for e, x in ranges:
        inside = entries[bisect.bisect_right(entries, e):bisect.bisect_left(entries, x)]
        if e in fns:
            exact += fns[e][0] == x - e and not inside
            pieces += len(inside)
        else:
            i = bisect.bisect_right(entries, e) - 1
            if i >= 0 and entries[i] + fns[entries[i]][0] > e:
                merged += 1
            else:
                missed += 1
    return exact, pieces, merged, missed


def tiers_of(path):
    """--tiers-tsv: entry -> (tier, gap code)"""
    out = {}
    for line in open(path):
        if line.startswith('#'):
            continue
        entry, tier, _words, gap = line.rstrip('\n').split('\t')
        out[int(entry, 16)] = (tier, gap == 'gap')
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nxaot', required=True)
    ap.add_argument('--work', required=True)
    a = ap.parse_args()
    shutil.rmtree(a.work, ignore_errors=True)
    os.makedirs(a.work)
    bad = 0

    def check(ok, what):
        nonlocal bad
        print(('  ' if ok else '  FAIL ') + what)
        bad += 0 if ok else 1

    mod = os.path.join(a.work, 'module')
    gen = run([sys.executable, GEN, '--direct', '--functions', str(FUNCTIONS), '--seed', str(SEED), '--no-fde-percent', str(PERCENT), mod]).strip()
    print(gen)
    total = int(re.match(r'gen_bench.py --direct: (\d+) functions', gen).group(1))
    manifest = os.path.join(mod, 'main.aot.tsv')
    nofde = [tuple(int(v, 16) for v in line.split()) for line in open(os.path.join(mod, 'main.nofde.tsv')) if not line.startswith('#')]
    stripped = round(FUNCTIONS * PERCENT / 100)
    fdes = eh_frame_hdr(open(os.path.join(mod, 'main.image.bin'), 'rb').read())
    print('the generator:')
    check(len(nofde) == stripped, f'{len(nofde)} functions without an FDE listed (want {stripped})')
    check(len(fdes) == total - stripped and fdes == sorted(fdes) and not {s for s, _ in fdes} & {e for e, _ in nofde},
          f'.eh_frame_hdr: {len(fdes)} FDEs of {total} functions, sorted, none at a stripped function (want {total - stripped})')

    # ---- Discovery ----
    tsv = os.path.join(a.work, 'discover.tsv')
    run([a.nxaot, 'discover', manifest, '--tsv', tsv])
    fns, summary = {}, ''
    for line in open(tsv):
        f = line.rstrip('\n').split('\t')
        if f[0] == 'fn':
            fns[int(f[1], 16)] = (int(f[2], 16), f[4])
        elif f[:2] == ['summary', 'provenance']:
            summary = f
    entries = sorted(fns)
    prov = collections.Counter(fns[e][1] for e, _ in nofde if e in fns)
    print(f'discovery: {len(fns)} functions; the {len(nofde)} without an FDE by provenance {dict(sorted(prov.items()))}')
    check(dict(prov) == WANT_PROV, f'their provenance (want {WANT_PROV})')
    for what, ranges in (('without an FDE', nofde), ('with an FDE', fdes)):
        exact, pieces, merged, missed = bounds(fns, entries, ranges)
        check(exact == len(ranges) and pieces == merged == missed == 0,
              f'{what}: {exact} of {len(ranges)} at their entry with their exact bounds, {pieces} pieces, {merged} merged into the function '
              f'before, {missed} missed')
    gap = summary[summary.index('gap') + 1] if 'gap' in summary else '-'
    check(gap == WANT_SUMMARY_GAP, f'summary provenance: gap {gap} (want {WANT_SUMMARY_GAP})')
    gap_code = {e for e, (size, p) in fns.items() if p == 'gap'}
    check(len(fns) == total + 1, f'{len(fns)} functions discovered: the {total} and the module entry\'s branch')

    # ---- The default (gap code at its mode's tier, W here) and --gap-tier C ----
    packs = {}
    for gap_tier, extra in (('W', []), ('C', ['--gap-tier', 'C'])):
        print(f'nxaot build --tier W{" " + " ".join(extra) if extra else ""}:')
        want_line = WANT_GAP_LINE + (' at tier C' if gap_tier == 'C' else '')
        out, logs, cache = {}, {}, os.path.join(a.work, f'cache-{gap_tier}')
        for workers in (1, 4):
            d = os.path.join(a.work, f'{gap_tier}-w{workers}')
            os.makedirs(d)
            cmd = [a.nxaot, 'build', manifest, '--shared', os.path.join(d, 'pack.so'), '--tier', 'W', '--workers', str(workers),
                   '--tiers-tsv', os.path.join(d, 'tiers.tsv')] + extra + (['--cache', cache] if workers == 4 else [])
            logs[workers] = run(cmd)
            out[workers] = (open(os.path.join(d, 'pack.so'), 'rb').read(), open(os.path.join(d, 'tiers.tsv')).read())
        log = logs[1]
        check(re.search('^' + re.escape(want_line) + '$', log, re.M) is not None, f'the log has "{want_line}" (the whole line)')
        check(re.search('^' + re.escape(WANT_TIERS[gap_tier]) + '$', log, re.M) is not None, f'the log has "{WANT_TIERS[gap_tier]}"')
        tiers = tiers_of(os.path.join(a.work, f'{gap_tier}-w1', 'tiers.tsv'))
        wrong = [e for e, (t, g) in tiers.items() if t != (gap_tier if e in gap_code else 'W')]
        marked = {e for e, (t, g) in tiers.items() if g}
        check(set(tiers) == set(fns) and not wrong and marked == gap_code,
              f'--tiers-tsv: {len(tiers)} functions, every gap function at {gap_tier} and every other at W ({len(wrong)} not), gap code marked '
              f'as discovery found it ({len(marked)} marked)')
        check(out[1][0] == out[4][0], f'the pack the same bytes with 1 and 4 workers ({len(out[1][0])} bytes)')
        check(out[1][1] == out[4][1], '--tiers-tsv the same with 1 and 4 workers')
        d = os.path.join(a.work, f'{gap_tier}-warm')
        os.makedirs(d)
        warm = run([a.nxaot, 'build', manifest, '--shared', os.path.join(d, 'pack.so'), '--tier', 'W', '--workers', '4',
                    '--tiers-tsv', os.path.join(d, 'tiers.tsv'), '--cache', cache] + extra)
        hit = re.search(r'^aot: analysis cache: hit', warm, re.M) and re.search(r'^aot: cache .*: \d+ hits, 0 misses', warm, re.M)
        check(bool(hit) and re.search('^' + re.escape(want_line) + '$', warm, re.M) is not None and
              open(os.path.join(d, 'pack.so'), 'rb').read() == out[4][0] and open(os.path.join(d, 'tiers.tsv')).read() == out[4][1],
              'a warm repack (the analysis and every unit from the cache): the same gap line, tiers and pack')
        packs[gap_tier] = out[1][0]
    check(packs['W'] != packs['C'], 'the default pack and the --gap-tier C pack differ')

    # ---- A profile that samples one gap function: the profile wins ----
    hot = min(gap_code)
    profile = os.path.join(a.work, 'one.profile')
    with open(profile, 'w') as f:
        f.write(f'# one gap function sampled\n{hot:#x} 5\n')
    d = os.path.join(a.work, 'profile')
    os.makedirs(d)
    log = run([a.nxaot, 'build', manifest, '--shared', os.path.join(d, 'pack.so'), '--profile', profile, '--workers', '4', '--gap-tier', 'C',
               '--tiers-tsv', os.path.join(d, 'tiers.tsv')])
    tiers = tiers_of(os.path.join(d, 'tiers.tsv'))
    m = re.search(r'^aot: gap code: (\d+) functions, (\d+) instructions at tier C; 1 functions, (\d+) instructions sampled by the profile at tier H$',
                  log, re.M)
    check(tiers[hot] == ('H', True) and sum(1 for t, g in tiers.values() if t == 'C') == len(tiers) - 1 and m and int(m.group(1)) == len(gap_code) - 1
          and 'aot: tiers C 4048 W 0 H 1 H+ 0 (functions)' in log,
          f'--gap-tier C, --profile sampling the gap function {hot:#x}: it at H, every other function at C, the log counting it apart')

    # ---- The max mode: every function at its mode's tier, --gap-tier C or not ----
    d = os.path.join(a.work, 'max')
    os.makedirs(d)
    log = run([a.nxaot, 'build', manifest, '--shared', os.path.join(d, 'pack.so'), '--mode', 'max', '--workers', '4', '--gap-tier', 'C',
               '--tiers-tsv', os.path.join(d, 'tiers.tsv')])
    tiers = tiers_of(os.path.join(d, 'tiers.tsv'))
    check(all(t == 'H+' for t, g in tiers.values()) and f'aot: tiers C 0 W 0 H 0 H+ {len(tiers)} (functions)' in log and
          re.search('^' + re.escape(WANT_GAP_LINE) + '$', log, re.M) is not None,
          '--mode max --gap-tier C: every function at H+, gap code included, and the gap line without "at tier C"')

    print(f'gap code: {len(nofde)} functions without an FDE, {len(gap_code)} gap code, at W by default and at C with --gap-tier C: {bad} failures')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
