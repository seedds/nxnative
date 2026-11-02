#!/usr/bin/env python3
"""The tables' object through MC (CTest `tables`; README.md, "The tables' object through MC"): `nxaot build` writes the function map's and the
lookup table's rows of the tables' object straight to the MC layer (src/build/tables.cpp) where the module has zero-initialised placeholders,
instead of LLVM constants against a declaration of every body and standard entry (the IR path, kept for --ir, --ir-opt and --asm, and behind
the test switch NXAOT_TABLES_IR). The object must be the same bytes, and so the pack. For each case below, one build by default and one with
NXAOT_TABLES_IR=1, each writing its tables' object to a file of the test switch NXAOT_TABLES_OBJECT:

- the two tables' objects the same bytes, and the two outputs (shared object, DLL or relocatable object) the same bytes; of a relocatable
  object (-o) every section but the units' line marks (`.nxaot_pcs` and its relocations: LLVM writes a unit's marks in an order that depends
  on its metadata nodes' addresses, so two runs of one binary differ there; nxaot reads them back by their relocations, and a shared object or
  a DLL has none of them: README.md, "The tables' object through MC");
- the default build's log says `aot: tables' object: <n> function map rows and <m> lookup slots through MC`, the other's `as LLVM constants`,
  with the same counts;

over corpus NROs (the default convention and ghccc, whose bodies may be their own standard entries), the synthetic game module of
dm/tools/tests/aot_synth.py (gate sites: the gate table and its wrapper) and a game-shaped module of `gen_bench.py --direct` (3,000 functions:
thousands of rows, the lookup table's probing), at tiers C, W and H+ (whose O3 module pipeline runs over the placeholders), for Linux (a shared
object, a relocatable object) and Windows (a DLL); and --ir, which takes the IR path by itself: its pack the same as the default build's.

  test_tables.py --nxaot <nxaot> --work <dir>

Exit 0 when every check holds.
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, '..', 'corpus')
TOOLS_TESTS = os.path.join(HERE, '..', '..', 'tests')
GEN = os.path.join(HERE, 'gen_bench.py')
LINE = re.compile(r"^aot: tables' object: (\d+) function map rows and (\d+) lookup slots (through MC|as LLVM constants), ", re.M)
LINE_MARKS = ('.nxaot_pcs', '.rela.nxaot_pcs')


def sections(elf):
    """An ELF64 object's sections as (name, header fields, bytes), in the section header table's order"""
    shoff, = struct.unpack_from('<Q', elf, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', elf, 0x3A)
    rows = [struct.unpack_from('<IIQQQQIIQQ', elf, shoff + i * shentsize) for i in range(shnum)]
    names = rows[shstrndx][4]
    out = []
    for name, typ, flags, addr, off, size, link, info, align, entsize in rows:
        n = elf[names + name:elf.index(b'\0', names + name)].decode()
        out.append((n, (typ, flags, addr, size, link, info, align, entsize), elf[off:off + size] if typ != 8 else b''))
    return out


def same_but_line_marks(a, b):
    """Two relocatable objects: the same section headers, and the same bytes in every section but the line marks'"""
    sa, sb = sections(a), sections(b)
    return [(n, h) for n, h, _ in sa] == [(n, h) for n, h, _ in sb] and all(
        x == y for (n, _, x), (_, _, y) in zip(sa, sb) if n not in LINE_MARKS)


def build(nxaot, module, out, args, ir_path):
    """One build into out (a folder of its own: the output's file name is its soname) by MC or by the IR path: (the tables' object, the
    output, the log's tables line)"""
    os.makedirs(out, exist_ok=True)
    env = dict(os.environ)
    env.pop('NXAOT_TABLES_IR', None)
    if ir_path:
        env['NXAOT_TABLES_IR'] = '1'
    env['NXAOT_TABLES_OBJECT'] = os.path.join(out, 'tables.o')
    windows = '--target' in args
    target = os.path.join(out, 'pack.dll' if windows else 'pack.so')
    output = ['--shared', target]
    if '-o' in args:
        args = [a for a in args if a != '-o']
        target = os.path.join(out, 'pack.o')
        output = ['-o', target]
    cmd = [nxaot, 'build', module] + output + args
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode != 0:
        raise SystemExit(f'test_tables.py: {" ".join(cmd)} failed (exit {r.returncode}):\n{r.stdout}{r.stderr}')
    m = LINE.search(r.stdout)
    with open(env['NXAOT_TABLES_OBJECT'], 'rb') as f:
        tables = f.read()
    with open(target, 'rb') as f:
        pack = f.read()
    return tables, pack, m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nxaot', required=True)
    ap.add_argument('--work', required=True)
    a = ap.parse_args()
    shutil.rmtree(a.work, ignore_errors=True)
    os.makedirs(a.work)
    sys.path.insert(0, TOOLS_TESTS)
    import aot_synth  # noqa: E402  (dm/tools/tests: the synthetic game module, as build_packs.sh writes it)
    aot_synth.write(os.path.join(a.work, 'inputs', 'module'))
    gen = subprocess.run([sys.executable, GEN, '--direct', '--functions', '3000', '--seed', '3', os.path.join(a.work, 'inputs', 'direct')],
                         capture_output=True, text=True)
    if gen.returncode != 0:
        raise SystemExit(f'test_tables.py: gen_bench.py failed:\n{gen.stdout}{gen.stderr}')
    fnptr = os.path.join(CORPUS, 'nro', 'fnptr-gcc.nro')
    switch = os.path.join(CORPUS, 'nro', 'switch-clang.nro')
    module = os.path.join(a.work, 'inputs', 'module', 'main.aot.tsv')
    direct = os.path.join(a.work, 'inputs', 'direct', 'main.aot.tsv')
    cases = [
        ('fnptr-gcc', fnptr, []),
        ('fnptr-gcc C', fnptr, ['--tier', 'C']),
        ('fnptr-gcc max', fnptr, ['--mode', 'max']),
        ('fnptr-gcc ghc', fnptr, ['--cc', 'ghc']),
        ('fnptr-gcc -o', fnptr, ['-o']),
        ('fnptr-gcc windows', fnptr, ['--target', 'windows']),
        ('switch-clang', switch, []),
        ('switch-clang ghc C', switch, ['--cc', 'ghc', '--tier', 'C']),
        ('module', module, []),
        ('module max', module, ['--mode', 'max']),
        ('module windows', module, ['--target', 'windows']),
        ('direct 3000', direct, []),
        ('direct 3000 C', direct, ['--tier', 'C']),
        ('direct 3000 -o', direct, ['-o']),
        ('direct 3000 windows', direct, ['--target', 'windows']),
    ]
    failures = 0
    print(f'{"case":<22} result (the tables\' object and the output through MC against the IR path)')
    for i, (name, path, args) in enumerate(cases):
        mc = build(a.nxaot, path, os.path.join(a.work, f'c{i}', 'mc'), args, False)
        ir = build(a.nxaot, path, os.path.join(a.work, f'c{i}', 'ir'), args, True)
        bad = []
        if mc[0] != ir[0]:
            bad.append(f'tables\' object differs ({len(mc[0])} and {len(ir[0])} bytes)')
        relocatable = '-o' in args
        if mc[1] != ir[1] and not (relocatable and same_but_line_marks(mc[1], ir[1])):
            bad.append(f'output differs ({len(mc[1])} and {len(ir[1])} bytes)')
        if not mc[2] or mc[2].group(3) != 'through MC':
            bad.append('the default build did not go through MC')
        if not ir[2] or ir[2].group(3) != 'as LLVM constants':
            bad.append('NXAOT_TABLES_IR did not take the IR path')
        if mc[2] and ir[2] and mc[2].group(1, 2) != ir[2].group(1, 2):
            bad.append('the counts differ')
        failures += len(bad)
        counts = f'{mc[2].group(1)} rows, {mc[2].group(2)} slots' if mc[2] else '?'
        same = 'identical' if mc[1] == ir[1] else 'identical but the line marks'
        print(f'{name:<22} {same if not bad else "FAIL: " + "; ".join(bad)} ({counts}, tables\' object {len(mc[0])} bytes, '
              f'output {len(mc[1])} bytes)')
        shutil.rmtree(os.path.join(a.work, f'c{i}'), ignore_errors=True)
    # --ir takes the IR path by itself (the listing shows the rows): the same pack as the default build's
    out = os.path.join(a.work, 'ir-listing')
    os.makedirs(out)
    so_mc = os.path.join(out, 'mc', 'pack.so')
    so_ir = os.path.join(out, 'ir', 'pack.so')
    os.makedirs(os.path.dirname(so_mc))
    os.makedirs(os.path.dirname(so_ir))
    r1 = subprocess.run([a.nxaot, 'build', fnptr, '--shared', so_mc], capture_output=True, text=True)
    r2 = subprocess.run([a.nxaot, 'build', fnptr, '--shared', so_ir, '--ir', os.path.join(out, 'pack.ll')], capture_output=True, text=True)
    m1, m2 = LINE.search(r1.stdout), LINE.search(r2.stdout)
    ok = (r1.returncode == 0 and r2.returncode == 0 and m1 and m2 and m1.group(3) == 'through MC' and m2.group(3) == 'as LLVM constants' and
          open(so_mc, 'rb').read() == open(so_ir, 'rb').read())
    failures += not ok
    print(f'{"fnptr-gcc --ir":<22} {"identical (--ir took the IR path)" if ok else "FAIL"}')
    print(f'tables\' object through MC: {len(cases) + 1} cases, {failures} failures')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
