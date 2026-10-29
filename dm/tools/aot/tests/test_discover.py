#!/usr/bin/env python3
"""The discovery test (CTest `discover`): `nxaot discover` over every corpus NRO (dm/tools/aot/corpus, corpus.tsv), compared with the full symbol
table of the NRO's ELF (corpus/nro/<nro>.syms.tsv: every STT_FUNC symbol, local ones included, and the assembler's mapping symbols), twice:
with the NRO's own DT_SYMTAB as a source (`symbols`; its exported functions) and with no symbol as a source (`--symbols-as-oracle`, `oracle`).

  test_discover.py --nxaot <path to nxaot> --work <scratch folder>

Checks, per NRO and mode:
- every STT_FUNC symbol of .symtab is the start of a discovered function (0 missed);
- no discovered entry lies inside another symbol's range (a function split) and every discovered entry has a symbol (an extra would have to be
  explained here: there is none in this corpus);
- sizes: the discovered size is the walk's extent, the symbol's size is the assembler's; they are compared as equal, or shorter by trailing
  words that are padding or traps (nop, zero, brk, udf: after a call that does not return, or alignment), and must never be longer;
- the coverage reports 0 uncovered words outside data blocks and 0 undefined words inside functions;
- every `$d` mapping symbol in the text lies in a data block the loader found (the NRO header, MOD0);
- the switch program has no br left for the dispatcher: every one is a jump table; fnptr's cleanup landing pads are found; excl's exclusive
  windows include an ABA-sensitive one.
It prints one row per NRO and mode: functions by primary provenance, jump tables by entry width, the brs left, windows, summary classes.
Exit 0 when every check passes.
"""
import argparse
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, '..', 'corpus')
NOP = 0xD503201F


def is_pad_or_trap(w):
    return w in (NOP, 0) or (w & 0xFFE0001F) in (0xD4200000, 0xD4400000) or (w >> 16) == 0


def symbols(path):
    funcs, data_maps = {}, []
    with open(path) as f:
        for line in f:
            if line.startswith('#'):
                continue
            value, size, kind, bind, name = line.rstrip('\n').split('\t')
            value, size = int(value, 16), int(size, 16)
            if kind == 'func':
                if value not in funcs or bind != 'local':
                    funcs[value] = (size, name)
            elif kind == 'map' and name.startswith('$d'):
                data_maps.append(value)
    return funcs, data_maps


def discover(nxaot, nro, tsv, oracle):
    cmd = [nxaot, 'discover', nro, '--tsv', tsv] + (['--symbols-as-oracle'] if oracle else [])
    run = subprocess.run(cmd, capture_output=True, text=True)
    if run.returncode:
        raise SystemExit(f'{" ".join(cmd)} failed:\n{run.stdout}{run.stderr}')
    fns, tables, brs, summary, windows = {}, [], [], {}, []
    with open(tsv) as f:
        for line in f:
            r = line.rstrip('\n').split('\t')
            if r[0] == 'fn':
                fns[int(r[1], 16)] = {'size': int(r[2], 16), 'prov': r[4], 'class': r[14], 'name': r[17]}
            elif r[0] == 'jt':
                tables.append({'br': int(r[1], 16), 'width': int(r[4])})
            elif r[0] == 'br':
                brs.append((int(r[1], 16), r[4]))
            elif r[0] == 'win':
                windows.append(r[4])
            elif r[0] == 'summary':
                summary[r[1]] = dict(zip(r[2::2], r[3::2]))
    return fns, tables, brs, summary, windows


def data_blocks(nxaot, nro):
    run = subprocess.run([nxaot, 'module', nro], capture_output=True, text=True, check=True)
    return [(int(r.split('\t')[1], 16), int(r.split('\t')[2], 16)) for r in run.stdout.splitlines() if r.startswith('data\t')]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nxaot', required=True)
    ap.add_argument('--work', required=True)
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    rows = []
    with open(os.path.join(CORPUS, 'corpus.tsv')) as f:
        for line in f:
            if not line.startswith('#') and line.strip():
                nro, program, toolchain = line.split('\t')[:3]
                rows.append((nro, program, toolchain))
    failures = []
    header = (f'{"nro":<18} {"mode":<7} {"fns":>4} {"provenance (primary)":<44} {"tables b/h/w":>12} {"br":>3} {"windows c/a/l":>13} '
              f'{"pres/open/nor":>13} {"pads":>4} {"miss":>4} {"extra":>5} {"split":>5} {"sizes =/<":>9} {"uncov":>5}')
    print(header)
    for nro, program, toolchain in rows:
        path = os.path.join(CORPUS, 'nro', nro)
        image = open(path, 'rb').read()
        funcs, data_maps = symbols(os.path.join(CORPUS, 'nro', nro[:-4] + '.syms.tsv'))
        blocks = data_blocks(a.nxaot, path)
        for addr in data_maps:
            if not any(lo <= addr < hi for lo, hi in blocks):
                failures.append(f'{nro}: the $d mapping symbol at {addr:#x} lies in no data block of the loader ({blocks})')
        ranges = sorted((v, v + s) for v, (s, _) in funcs.items())
        for mode in ('symbols', 'oracle'):
            fns, tables, brs, summary, windows = discover(a.nxaot, path, os.path.join(a.work, f'{nro}.{mode}.tsv'), mode == 'oracle')
            tag = f'{nro} ({mode})'
            missed = [(v, n) for v, (s, n) in funcs.items() if v not in fns]
            for v, n in missed:
                failures.append(f'{tag}: the symbol {n} at {v:#x} is no discovered function')
            extra = [e for e in fns if e not in funcs]
            splits = [e for e in fns if any(lo < e < hi for lo, hi in ranges)]
            for e in extra:
                failures.append(f'{tag}: the discovered function at {e:#x} ({fns[e]["prov"]}) has no symbol')
            for e in splits:
                failures.append(f'{tag}: the discovered function at {e:#x} ({fns[e]["prov"]}) lies inside a symbol\'s range')
            same = shorter = 0
            for v, (s, n) in funcs.items():
                if v not in fns:
                    continue
                d = fns[v]['size']
                if d == s:
                    same += 1
                elif d < s and all(is_pad_or_trap(struct.unpack_from('<I', image, p)[0]) for p in range(v + d, v + s, 4)):
                    shorter += 1
                else:
                    failures.append(f'{tag}: {n} at {v:#x}: discovered size {d:#x}, symbol size {s:#x}, and the difference is not padding or traps')
            cov = summary.get('coverage', {})
            if cov.get('uncovered') != '0' or cov.get('undefined') != '0':
                failures.append(f'{tag}: coverage {cov}')
            if program == 'switch' and brs:
                failures.append(f'{tag}: brs left for the dispatcher in the switch program: {[f"{b:#x} ({w})" for b, w in brs]}')
            pads = int(summary.get('discovery', {}).get('landing_pads', '0'))
            if program == 'fnptr' and pads == 0:
                failures.append(f'{tag}: no landing pad found in a program built with -fexceptions')
            if program == 'excl' and 'aba' not in windows:
                failures.append(f'{tag}: the ABA-sensitive window of the lock-free pop was not found')
            primary = {}
            for fn in fns.values():
                p = fn['prov'].split(',')[0]
                primary[p] = primary.get(p, 0) + 1
            prov = ','.join(f'{k}:{primary[k]}' for k in sorted(primary, key=lambda k: -primary[k]))
            widths = [sum(1 for t in tables if t['width'] == w) for w in (1, 2, 4)]
            classes = [sum(1 for f in fns.values() if f['class'] == c) for c in ('preserving', 'open', 'noreturn')]
            wins = [windows.count(k) for k in ('cas', 'aba', 'load')]
            print(f'{nro:<18} {mode:<7} {len(fns):>4} {prov:<44} {"/".join(map(str, widths)):>12} {len(brs):>3} {"/".join(map(str, wins)):>13} '
                  f'{"/".join(map(str, classes)):>13} {pads:>4} {len(missed):>4} {len(extra):>5} {len(splits):>5} {f"{same}/{shorter}":>9} '
                  f'{cov.get("uncovered", "?"):>5}')
    for line in failures:
        print('FAIL ' + line)
    print(f'discover test: {len(rows)} NROs, 2 modes each, {len(failures)} failures')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
