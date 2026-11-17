#!/usr/bin/env python3
"""The pack stage's caches (CTest `cache`; docs/design/aot-recompiler.md section 13, README.md "Pack throughput"): the per-function cache of compiled
units and the analysis cache.

Every NRO of corpus.tsv packed into a new cache (every unit a miss, every function in one, the analysis computed on the workers and stored),
then again (every unit a hit, nothing compiled, the analysis read from the cache and not run, and the shared object byte for byte the cold
one's, also with another number of workers); then one NRO with one instruction word changed (a movz immediate in a function without a jump
table, so that discovery and the convention facts stay the same, which `nxaot discover --tsv` confirms) misses one unit only, the group that
holds that function, and its analysis (another image), which runs again on the workers and writes the entry a pack with one worker writes
(README.md, "Phase 5c"); the same NRO at another tier misses every unit and takes the analysis from the cache (the analysis does not depend on
the tier); a second pack of it with a profile of its largest function keeps the first pack's tier W for the functions whose units the cache holds
(README.md, "The second pack's cached units"), and with NXAOT_FIRST_PACK_UNITS=0 none. Then the analysis cache's entries are never trusted when they cannot be: an entry with a byte changed (its checksum fails), cut short,
or with a payload that does not parse (with a valid checksum) is ignored and written again, and each such pack is byte for byte the cold one.

  test_cache.py --nxaot <nxaot> --work <dir> [--nro <name>]... [--changed <name>]

Exit 0 when every check holds.
"""
import argparse
import glob
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, '..', 'corpus')


class Pack:
    def __init__(self, functions, hits, misses, fn_hits, fn_misses, analysis, analysis_threads):
        self.functions, self.hits, self.misses, self.fn_hits, self.fn_misses, self.analysis = (functions, hits, misses, fn_hits, fn_misses,
                                                                                              analysis)
        self.analysis_threads = analysis_threads  # the threads the analysis ran on; 0 when the analysis cache held it


def build(nxaot, path, so, cache, *extra):
    r = subprocess.run([nxaot, 'build', path, '--shared', so, '--cache', cache] + list(extra), capture_output=True, text=True)
    m = re.search(r'^aot: \S+ \d+ instructions (\d+) functions$', r.stdout, re.M)
    c = re.search(r'^aot: cache \S+: (\d+) hits, (\d+) misses \(units; functions (\d+) hit, (\d+) missed\)', r.stdout, re.M)
    an = re.search(r'^aot: analysis cache: (.*), [0-9.]+ s$', r.stdout, re.M)
    t = re.search(r'^aot: analysis on (\d+) threads', r.stdout, re.M)
    if r.returncode != 0 or not m or not c or not an:
        print(r.stdout + r.stderr)
        raise SystemExit(f'test_cache.py: nxaot build {path} failed (exit {r.returncode})')
    return Pack(int(m.group(1)), int(c.group(1)), int(c.group(2)), int(c.group(3)), int(c.group(4)), an.group(1), int(t.group(1)) if t else 0)


def discover(nxaot, path, tsv):
    subprocess.run([nxaot, 'discover', path, '--tsv', tsv], capture_output=True, check=True)
    rows = open(tsv).read().splitlines()
    # The fn, jt, br, call, win rows and the summaries: everything but the regions-lifted count (work done, not a result)
    return [re.sub(r'\tregions_lifted\t\d+', '', r) for r in rows if not r.startswith('#')]


def changed_word(nxaot, nro, work):
    """A copy of the NRO with one movz immediate changed, in a function with no jump table, discovery's tables otherwise unchanged: the copy's
    path and the function's entry"""
    base_tsv = os.path.join(work, 'base.discover.tsv')
    rows = discover(nxaot, nro, base_tsv)
    with_tables = {int(r.split('\t')[2], 16) for r in rows if r.startswith('jt\t')}
    fns = []
    for r in rows:
        if r.startswith('fn\t'):
            f = r.split('\t')
            fns.append((int(f[1], 16), int(f[2], 16)))
    data = bytearray(open(nro, 'rb').read())
    text_off = 0  # an NRO's text starts the file
    text_size = struct.unpack_from('<I', data, 0x10 + 0x10 + 4)[0]  # the header's first segment: offset, size
    os.makedirs(os.path.join(work, 'changed'), exist_ok=True)
    copy = os.path.join(work, 'changed', os.path.basename(nro))  # the same file name: the module's name, part of every key, stays
    for entry, size in fns:
        if entry in with_tables:
            continue
        for pc in range(entry, entry + size, 4):
            if pc < text_off or pc + 4 > text_off + text_size:
                break
            w = struct.unpack_from('<I', data, pc)[0]
            if (w & 0x7F800000) != 0x52800000:  # movz w/x, #imm16
                continue
            trial = bytearray(data)
            struct.pack_into('<I', trial, pc, w ^ (1 << 5))  # the immediate's lowest bit
            with open(copy, 'wb') as f:
                f.write(trial)
            if discover(nxaot, copy, os.path.join(work, 'changed.discover.tsv')) == rows:
                return copy, entry, pc
    raise SystemExit(f'test_cache.py: no movz in a function without a jump table of {nro} leaves discovery unchanged')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nxaot', required=True)
    ap.add_argument('--work', required=True)
    ap.add_argument('--nro', action='append', default=[])
    ap.add_argument('--changed', default='fnptr-gcc.nro')
    a = ap.parse_args()
    shutil.rmtree(a.work, ignore_errors=True)
    os.makedirs(a.work)
    cache = os.path.join(a.work, 'aot-cache')
    nros = []
    with open(os.path.join(CORPUS, 'corpus.tsv')) as f:
        for line in f:
            if not line.startswith('#') and line.strip():
                nros.append(line.split('\t')[0])
    if a.nro:
        nros = [n for n in nros if n in a.nro]
    bad = 0
    for d in ('cold', 'warm'):
        os.makedirs(os.path.join(a.work, d))
    total = 0
    print(f'{"nro":<20} {"functions":>9} {"cold units hit/miss":>19} {"warm units hit/miss":>19} {"analysis cold, warm":<22}  pack')
    for nro in nros:
        path = os.path.join(CORPUS, 'nro', nro)
        cold = os.path.join(a.work, 'cold', nro[:-4] + '.so')
        warm = os.path.join(a.work, 'warm', nro[:-4] + '.so')
        c = build(a.nxaot, path, cold, cache)
        w = build(a.nxaot, path, warm, cache, '--workers', '1')
        same = open(cold, 'rb').read() == open(warm, 'rb').read()
        ok = (c.hits == 0 and c.fn_hits == 0 and c.fn_misses == c.functions and c.analysis.startswith('miss') and w.misses == 0 and
              w.fn_hits == w.functions == c.functions and w.fn_misses == 0 and w.analysis == 'hit' and same and c.analysis_threads > 0 and
              w.analysis_threads == 0)
        bad += not ok
        total += c.functions
        print(f'{nro:<20} {c.functions:>9} {f"{c.hits}/{c.misses}":>19} {f"{w.hits}/{w.misses}":>19} {c.analysis[:4] + ", " + w.analysis:<22}  '
              f'{"identical" if same else "DIFFERS"}{"" if ok else "  FAIL"}')
    print(f'cold and warm: {len(nros)} NROs, {total} functions')

    # One instruction word changed: only the unit that holds its function misses, and the analysis (another image), which runs again on the
    # workers (README.md, "Phase 5c"): its entry is the one a pack with one worker writes
    src = os.path.join(CORPUS, 'nro', a.changed)
    copy, entry, pc = changed_word(a.nxaot, src, a.work)
    p = build(a.nxaot, copy, os.path.join(a.work, 'changed.so'), cache, '--workers', '4')
    ok = (p.misses == 1 and p.fn_misses >= 1 and p.fn_hits + p.fn_misses == p.functions and p.analysis == 'miss (none)' and
          p.analysis_threads == 4)
    entries = {}
    for workers in (1, 4):
        own = os.path.join(a.work, f'changed-w{workers}')
        build(a.nxaot, copy, os.path.join(own, 'changed.so'), own, '--workers', str(workers))
        entries[workers] = [open(e, 'rb').read() for e in sorted(glob.glob(os.path.join(own, 'analysis', '*.bin')))]
    same_entry = len(entries[1]) == 1 and entries[1] == entries[4]
    ok = ok and same_entry
    bad += not ok
    print(f'changed word at {pc:#x} (the function at {entry:#x}) of {a.changed}: {p.functions} functions, units {p.hits} hits, {p.misses} misses '
          f'({p.fn_misses} functions in the missed unit); analysis {p.analysis} on {p.analysis_threads} threads, its entry '
          f'{"the same" if same_entry else "NOT the same"} with 1 worker{"" if ok else "  FAIL"}')

    # Another tier: every unit misses; the analysis is the same
    p = build(a.nxaot, src, os.path.join(a.work, 'tier_c.so'), cache, '--tier', 'C')
    ok = p.hits == 0 and p.fn_misses == p.functions and p.analysis == 'hit'
    bad += not ok
    print(f'tier C of {a.changed}: {p.functions} functions, units {p.hits} hits, {p.misses} misses; analysis {p.analysis}{"" if ok else "  FAIL"}')

    # The second pack (README.md, "The second pack's cached units"): a profile that samples the largest function only; the others keep the first
    # pack's tier W where its units (cached by the cold pack above) hold them, so the pack compiles little; with NXAOT_FIRST_PACK_UNITS=0 none keeps it
    rows = discover(a.nxaot, src, os.path.join(a.work, 'second.tsv'))
    fns = [r.split('\t') for r in rows if r.startswith('fn\t')]
    hot = max(fns, key=lambda f: int(f[2], 16))
    prof = os.path.join(a.work, 'second.profile')
    with open(prof, 'w') as f:
        f.write(f'{hot[1]} 100\n')
    second = {}
    for name, env in (('kept', None), ('off', '0')):
        r = subprocess.run([a.nxaot, 'build', src, '--shared', os.path.join(a.work, f'second_{name}.so'), '--cache', cache, '--profile', prof],
                           capture_output=True, text=True, env=dict(os.environ, NXAOT_FIRST_PACK_UNITS=env) if env else None)
        m = re.search(r'^aot: second pack: (\d+) functions', r.stdout, re.M)
        c = re.search(r'^aot: cache \S+: (\d+) hits, (\d+) misses', r.stdout, re.M)
        second[name] = (int(m.group(1)) if m else -1, int(c.group(1)) if c else -1, int(c.group(2)) if c else -1, r.returncode)
    kept, off = second['kept'], second['off']
    ok = kept[3] == 0 and off[3] == 0 and 0 < kept[0] < len(fns) and off[0] == -1 and kept[1] > 0
    bad += not ok
    print(f'second pack of {a.changed} (a profile of its largest function, {hot[1]}): {kept[0]} of {len(fns)} functions keep tier W, units {kept[1]} hits, '
          f'{kept[2]} misses; with NXAOT_FIRST_PACK_UNITS=0 units {off[1]} hits, {off[2]} misses{"" if ok else "  FAIL"}')

    # The analysis cache's entries that cannot be trusted: changed, short, unparsable with a valid checksum. Each is ignored (a miss saying why),
    # the pack is the cold one byte for byte, and the entry is written again (the next pack hits)
    own = os.path.join(a.work, 'analysis-cache')
    base = os.path.join(a.work, 'analysis.cold.so')
    build(a.nxaot, src, base, own)
    entries = glob.glob(os.path.join(own, 'analysis', '*.bin'))
    if len(entries) != 1:
        bad += 1
        print(f'analysis cache: {len(entries)} entries after one pack  FAIL')
    else:
        entry_path = entries[0]
        good = open(entry_path, 'rb').read()
        head = good.index(b'\n') + 1 + 64  # the format line, then the key
        flipped = bytearray(good)
        flipped[len(good) // 2] ^= 0x40
        payload = good[:head] + b'\xff' * 10
        cases = [('a changed byte', bytes(flipped), 'miss (its checksum fails)'),
                 ('cut short', good[:len(good) // 2], 'miss (its checksum fails)'),
                 ('no parse, valid checksum', payload + hashlib.sha256(payload).digest(), 'miss (it does not parse)'),
                 ('another key', good[:head - 1] + bytes([good[head - 1] ^ 1]) + good[head:], 'miss (another format or key)')]
        for what, data, want in cases:
            with open(entry_path, 'wb') as f:
                f.write(data)
            os.makedirs(os.path.join(a.work, 'case'), exist_ok=True)
            so = os.path.join(a.work, 'case', os.path.basename(base))  # the same file name: the soname, written into the pack, stays
            p = build(a.nxaot, src, so, own)
            again = build(a.nxaot, src, so, own)
            same = open(so, 'rb').read() == open(base, 'rb').read()
            ok = p.analysis == want and p.misses == 0 and again.analysis == 'hit' and same
            bad += not ok
            print(f'analysis cache, an entry {what}: {p.analysis}, then {again.analysis}; pack {"identical" if same else "DIFFERS"}'
                  f'{"" if ok else "  FAIL"}')
    bad += prune_checks(a.nxaot, src, a.work)
    print(f'pack caches: {len(nros)} NROs cold and warm, a changed word, a changed tier, untrusted analysis entries, the cache pruned: {bad} failures')
    return 1 if bad else 0


def prune_checks(nxaot, src, work):
    """`nxaot cache-prune` (src/cache/cache.h; README.md, "The cache's size"): a hit refreshes an entry's time; a tool's own entries stay whatever their
    age; the entries of older tools (last used before this tool's first use of the cache) go once they were not used for the given time, with objects
    whose facts never came, interrupted packs' temporary files and analysis entries; a tool that never packed into a cache removes nothing"""
    bad = 0
    cache = os.path.join(work, 'prune-cache')
    shutil.rmtree(cache, ignore_errors=True)
    so = os.path.join(work, 'prune', os.path.basename(src) + '.so')
    os.makedirs(os.path.dirname(so), exist_ok=True)
    cold = build(nxaot, src, so, cache)
    facts = sorted(glob.glob(os.path.join(cache, '[0-9a-f][0-9a-f]', '*.tsv')))
    marks = glob.glob(os.path.join(cache, 'tools', '*'))
    day = 86400

    def prune(seconds, where=cache):
        r = subprocess.run([nxaot, 'cache-prune', where, str(seconds)], capture_output=True, text=True)
        m = re.search(r'(\d+) units, (\d+) analyses and (\d+) partial files of older tools, not used for \d+ s, removed \((\d+) bytes\); (\d+) units kept', r.stdout)
        return r, tuple(int(x) for x in m.groups()) if m else None

    def age(path, days):
        at = time.time() - days * day
        os.utime(path, (at, at))

    def check(what, ok, got):
        nonlocal bad
        bad += not ok
        print(f'cache-prune, {what}: {got}{"" if ok else "  FAIL"}')

    check('a pack marks its tool', len(marks) == 1 and len(facts) == cold.misses and cold.misses > 2, f'{len(marks)} mark, {len(facts)} entries of {cold.misses} units')
    r, got = prune(0)
    check('the tool\'s own entries, no time allowed', got == (0, 0, 0, 0, len(facts)), r.stdout.strip())
    # a hit refreshes the time of an entry not used for over an hour
    for f in facts:
        age(f, 3)
    warm = build(nxaot, src, so, cache)
    fresh = sum(os.path.getmtime(f) > time.time() - 600 for f in facts)
    check('a hit refreshes an entry\'s time', warm.misses == 0 and fresh == len(facts), f'{warm.hits} hits, {fresh} of {len(facts)} refreshed')
    # an older tool's leftovers: its mark and entries older than this tool's mark (the mark is a day old, they are three)
    for m in marks:
        age(m, 1)
    old = facts[:len(facts) // 2]
    for f in old:
        age(f, 3)
    stale_key = 'f' * 64
    shard = os.path.join(cache, 'ff')
    os.makedirs(shard, exist_ok=True)
    extra = {'entry': [os.path.join(shard, stale_key + '.tsv'), os.path.join(shard, stale_key + '.o')], 'orphan': [os.path.join(shard, 'e' * 64 + '.o')],
             'temporary': [os.path.join(shard, 'd' * 64 + '.o.tmp.1.2')], 'analysis': [os.path.join(cache, 'analysis', 'c' * 64 + '.bin')],
             'other': [os.path.join(cache, 'notes.txt'), os.path.join(shard, 'README')]}   # files the cache never wrote: left alone
    os.makedirs(os.path.join(cache, 'analysis'), exist_ok=True)
    for paths in extra.values():
        for q in paths:
            with open(q, 'wb') as f:
                f.write(b'x' * 100)
            age(q, 3)
    r, got = prune(10 * day)
    check('ten days allowed', got == (0, 0, 0, 0, len(facts) + 1), r.stdout.strip())
    r, got = prune(3600)
    gone = [f for f in old if not os.path.exists(f) and not os.path.exists(f[:-4] + '.o')]
    left = {k: sum(os.path.exists(q) for q in v) for k, v in extra.items()}
    ok = got is not None and got[0] == len(old) + 1 and got[1] == 1 and got[2] == 2 and got[3] > 0 and got[4] == len(facts) - len(old) and \
        len(gone) == len(old) and left == {'entry': 0, 'orphan': 0, 'temporary': 0, 'analysis': 0, 'other': 2}
    check('an hour allowed', ok, f'{r.stdout.strip()}; left {left}')
    again = build(nxaot, src, so, cache)
    check('the pack after it', again.misses == len(old) and again.hits == len(facts) - len(old), f'{again.hits} hits, {again.misses} misses')
    # a tool that never packed into a cache: its mark is made, nothing is removed
    for m in glob.glob(os.path.join(cache, 'tools', '*')):
        os.remove(m)
    for f in glob.glob(os.path.join(cache, '[0-9a-f][0-9a-f]', '*.tsv')):
        age(f, 3)
    n = len(glob.glob(os.path.join(cache, '[0-9a-f][0-9a-f]', '*.tsv')))
    r, _ = prune(0)
    after = len(glob.glob(os.path.join(cache, '[0-9a-f][0-9a-f]', '*.tsv')))
    check('a first use', 'first use' in r.stdout and after == n and len(glob.glob(os.path.join(cache, 'tools', '*'))) == 1, r.stdout.strip())
    r = subprocess.run([nxaot, 'cache-prune', os.path.join(work, 'no-such-cache'), '0'], capture_output=True, text=True)
    check('no directory', r.returncode == 0, r.stdout.strip())
    return bad


if __name__ == '__main__':
    sys.exit(main())
