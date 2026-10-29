#!/usr/bin/env python3
"""The last step of build_corpus.sh: the corpus's ELF files (from compile.sh) become NROs with their symbol tables (tests/elf2nro.py), and
corpus.tsv lists every NRO with its program, toolchain, compiler, flags and SHA-256.

  nros.py <elf dir of gcc> <elf dir of clang> <out dir> <corpus.tsv>   write <out dir>/<program>-<toolchain>.nro and .syms.tsv, and the table
  nros.py --compare <dir a> <tsv a> <dir b> <tsv b>                     exit 1 unless both hold the same corpus files byte for byte
"""
import argparse
import hashlib
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ELF2NRO = os.path.join(HERE, '..', 'tests', 'elf2nro.py')
TOOLCHAINS = ('gcc', 'clang')


def read_flags(elf_dir):
    rows = []
    with open(os.path.join(elf_dir, 'flags.tsv')) as f:
        for line in f:
            if line.startswith('#') or not line.strip():
                continue
            program, tc, compiler, flags, link = line.rstrip('\n').split('\t')
            rows.append((program, tc, compiler, flags, link))
    return rows


def build(elf_dirs, out, tsv):
    os.makedirs(out, exist_ok=True)
    table = ['# nro\tprogram\ttoolchain\tcompiler\tsha256\tsize\tflags\tlink']
    for tc, elf_dir in zip(TOOLCHAINS, elf_dirs):
        for program, tc_row, compiler, flags, link in read_flags(elf_dir):
            assert tc_row == tc, (tc_row, tc)
            name = f'{program}-{tc}'
            nro = os.path.join(out, name + '.nro')
            subprocess.run([sys.executable, ELF2NRO, os.path.join(elf_dir, program + '.elf'), nro, '--symbols',
                            os.path.join(out, name + '.syms.tsv')], check=True)
            data = open(nro, 'rb').read()
            table.append(f'{name}.nro\t{program}\t{tc}\t{compiler}\t{hashlib.sha256(data).hexdigest()}\t{len(data)}\t{flags}\t{link}')
    with open(tsv, 'w', newline='\n') as f:
        f.write('\n'.join(table) + '\n')
    print(f'nros.py: {len(table) - 1} NROs in {out}')


def corpus_files(d):
    return sorted(f for f in os.listdir(d) if f.endswith(('.nro', '.syms.tsv')))


def compare(a, tsv_a, b, tsv_b):
    fa, fb = corpus_files(a), corpus_files(b)
    bad = [f'only in {a}: {f}' for f in fa if f not in fb] + [f'only in {b}: {f}' for f in fb if f not in fa]
    for f in fa:
        if f in fb and open(os.path.join(a, f), 'rb').read() != open(os.path.join(b, f), 'rb').read():
            bad.append(f'differs: {f}')
    if open(tsv_a, 'rb').read() != open(tsv_b, 'rb').read():
        bad.append(f'differs: {tsv_a} and {tsv_b}')
    for line in bad:
        print(line)
    print(f'nros.py --compare: {len(fa) + 1} files, {len(bad)} differences')
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--compare', nargs=4, metavar='PATH')
    ap.add_argument('dirs', nargs='*')
    a = ap.parse_args()
    if a.compare:
        return compare(*a.compare)
    if len(a.dirs) != 4:
        ap.error('expected <elf dir of gcc> <elf dir of clang> <out dir> <corpus.tsv>')
    build(a.dirs[:2], a.dirs[2], a.dirs[3])
    return 0


if __name__ == '__main__':
    sys.exit(main())
