#!/usr/bin/env python3
"""The pack stage's caches (CTest `cache`; docs/design/aot-recompiler.md section 13, README.md "Pack throughput"): the per-function cache of compiled
units and the analysis cache.

Every NRO of corpus.tsv packed into a new cache (every unit a miss, every function in one, the analysis computed and stored), then again (every
unit a hit, nothing compiled, the analysis read from the cache, and the shared object byte for byte the cold one's, also with another number of
workers); then one NRO with one instruction word changed (a movz immediate in a function without a jump table, so that discovery and the
convention facts stay the same, which `nxaot discover --tsv` confirms) misses one unit only, the group that holds that function, and its
analysis (another image); the same NRO at another tier misses every unit and takes the analysis from the cache (the analysis does not depend on
the tier). Then the analysis cache's entries are never trusted when they cannot be: an entry with a byte changed (its checksum fails), cut short,
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

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, '..', 'corpus')


class Pack:
    def __init__(self, functions, hits, misses, fn_hits, fn_misses, analysis):
        self.functions, self.hits, self.misses, self.fn_hits, self.fn_misses, self.analysis = (functions, hits, misses, fn_hits, fn_misses,
                                                                                              analysis)


def build(nxaot, path, so, cache, *extra):
    r = subprocess.run([nxaot, 'build', path, '--shared', so, '--cache', cache] + list(extra), capture_output=True, text=True)
    m = re.search(r'^aot: \S+ \d+ instructions (\d+) functions$', r.stdout, re.M)
    c = re.search(r'^aot: cache \S+: (\d+) hits, (\d+) misses \(units; functions (\d+) hit, (\d+) missed\)', r.stdout, re.M)
    an = re.search(r'^aot: analysis cache: (.*), [0-9.]+ s$', r.stdout, re.M)
    if r.returncode != 0 or not m or not c or not an:
        print(r.stdout + r.stderr)
        raise SystemExit(f'test_cache.py: nxaot build {path} failed (exit {r.returncode})')
    return Pack(int(m.group(1)), int(c.group(1)), int(c.group(2)), int(c.group(3)), int(c.group(4)), an.group(1))


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
              w.fn_hits == w.functions == c.functions and w.fn_misses == 0 and w.analysis == 'hit' and same)
        bad += not ok
        total += c.functions
        print(f'{nro:<20} {c.functions:>9} {f"{c.hits}/{c.misses}":>19} {f"{w.hits}/{w.misses}":>19} {c.analysis[:4] + ", " + w.analysis:<22}  '
              f'{"identical" if same else "DIFFERS"}{"" if ok else "  FAIL"}')
    print(f'cold and warm: {len(nros)} NROs, {total} functions')

    # One instruction word changed: only the unit that holds its function misses, and the analysis (another image)
    src = os.path.join(CORPUS, 'nro', a.changed)
    copy, entry, pc = changed_word(a.nxaot, src, a.work)
    p = build(a.nxaot, copy, os.path.join(a.work, 'changed.so'), cache)
    ok = p.misses == 1 and p.fn_misses >= 1 and p.fn_hits + p.fn_misses == p.functions and p.analysis == 'miss (none)'
    bad += not ok
    print(f'changed word at {pc:#x} (the function at {entry:#x}) of {a.changed}: {p.functions} functions, units {p.hits} hits, {p.misses} misses '
          f'({p.fn_misses} functions in the missed unit); analysis {p.analysis}{"" if ok else "  FAIL"}')

    # Another tier: every unit misses; the analysis is the same
    p = build(a.nxaot, src, os.path.join(a.work, 'tier_c.so'), cache, '--tier', 'C')
    ok = p.hits == 0 and p.fn_misses == p.functions and p.analysis == 'hit'
    bad += not ok
    print(f'tier C of {a.changed}: {p.functions} functions, units {p.hits} hits, {p.misses} misses; analysis {p.analysis}{"" if ok else "  FAIL"}')

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
    print(f'pack caches: {len(nros)} NROs cold and warm, a changed word, a changed tier, untrusted analysis entries: {bad} failures')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
