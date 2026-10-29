#!/usr/bin/env python3
"""The manifest test (CTest `manifest`): the parser and writer of design section 1.2 (src/module/manifest.h), and the behaviour-image form.

  test_manifest.py --nxaot <path to nxaot> --work <scratch folder>

- every row kind round-trips: `nxaot manifest` prints manifest_rows.aot.tsv (every kind of 1.2, in canonical form) back unchanged, and a
  non-canonical spelling of it (hex without 0x and in capitals, CRLF line ends, comments, blank lines, kinds out of order) as the same text;
- every malformed row is refused with its line number;
- one input form: for every corpus NRO, `nxaot image` writes <name>.image.bin and <name>.aot.tsv, and `nxaot discover` of the manifest gives
  the rows `nxaot discover` of the NRO gives (all but the header, which names the module).
Exit 0 when every check passes.
"""
import argparse
import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, '..', 'corpus')
ROWS = os.path.join(HERE, 'manifest_rows.aot.tsv')
HEADER = '# nxaot-manifest 1\tmain\t00\t' + '0' * 64 + '\n'

# Malformed rows: each must be refused, naming line 2
BAD = [
    'layout\ttext_end\t0x10\tro_off\t0x20\tdata_off\t0x30\tbss\t0x0',            # a field short
    'layout\ttext_end\t0x40\tro_off\t0x20\tdata_off\t0x30\tbss\t0x0\timage_size\t0x40',  # text_end past ro_off
    'aux\t0x10\t0x20\tfps\t0x30',                                                 # the keyword
    'data\t0x80\t0x40',                                                           # end before start
    'data\t0x80\tzz',                                                             # not hex
    'obs\t0x10\t0x16\t0x0\t0xd503201f',                                           # the number is decimal
    'obs\t0x10\t22\t0x0\t0x1d503201f',                                            # a word of more than 32 bits
    'spin\t0x10\t0x20,,0x30',                                                     # an empty back-edge
    'cut\t0x10\t1f20\t1f2003d5\twhy',                                             # byte strings of two lengths
    'cut\t0x10\t1f2\t1f2\twhy',                                                   # an odd number of digits
    'oracle-T\t0x10',                                                             # no targets
    'oracle-J\t0x10\t0x20\t3',                                                    # width 1 or 2
    'oracle-W\t0x20\t0x10',                                                       # end before start
    'jump\t0x10',                                                                 # an unknown kind
    'data\t0x11111111111111111\t0x2',                                             # more than 64 bits
]


def manifest(nxaot, path):
    return subprocess.run([nxaot, 'manifest', path], capture_output=True, text=True)


def scramble(text):
    """The same manifest spelled otherwise: no 0x, capitals, CRLF, a comment and blank lines, the kinds in another order"""
    lines = text.splitlines()
    body = []
    for line in lines[1:]:
        f = line.split('\t')
        out = []
        for i, x in enumerate(f):
            if i and re.fullmatch(r'0x[0-9a-f]+', x):
                x = x[2:].upper()
            elif i and re.fullmatch(r'(0x[0-9a-f]+,)+0x[0-9a-f]+', x):
                x = ','.join(v[2:] for v in x.split(','))
            out.append(x)
        body.append('\t'.join(out))
    kinds = {}
    for line in body:
        kinds.setdefault(line.split('\t')[0], []).append(line)
    order = list(kinds)
    random.Random(5).shuffle(order)
    shuffled = [line for k in order for line in kinds[k]]
    return '\r\n'.join([lines[0], '# a comment row', ''] + shuffled + ['']) + '\r\n'


def discover_rows(nxaot, path, tsv):
    subprocess.run([nxaot, 'discover', path, '--tsv', tsv], capture_output=True, text=True, check=True)
    return [line for line in open(tsv).read().splitlines() if not line.startswith('# nxaot discover')]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nxaot', required=True)
    ap.add_argument('--work', required=True)
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    failures = []
    canonical = open(ROWS).read()
    kinds = sorted({line.split('\t')[0] for line in canonical.splitlines()[1:]})
    run = manifest(a.nxaot, ROWS)
    if run.returncode or run.stdout != canonical:
        failures.append(f'manifest_rows.aot.tsv does not round-trip:\n{run.stdout}{run.stderr}')
    scrambled = os.path.join(a.work, 'scrambled.aot.tsv')
    with open(scrambled, 'w', newline='') as f:
        f.write(scramble(canonical))
    run = manifest(a.nxaot, scrambled)
    if run.returncode or run.stdout != canonical:
        failures.append(f'the scrambled spelling does not give the canonical text:\n{run.stdout}{run.stderr}')
    refused = 0
    for i, row in enumerate(BAD):
        path = os.path.join(a.work, f'bad{i}.aot.tsv')
        with open(path, 'w', newline='\n') as f:
            f.write(HEADER + row + '\n')
        run = manifest(a.nxaot, path)
        if run.returncode != 1 or 'line 2:' not in run.stdout:
            failures.append(f'the malformed row {row!r} was not refused with its line ({run.returncode}): {run.stdout.strip()}')
        else:
            refused += 1
    print(f'manifest rows: {len(canonical.splitlines()) - 1} rows of {len(kinds)} kinds ({", ".join(kinds)}) round-trip; '
          f'{refused} of {len(BAD)} malformed rows refused with their line')

    nros = sorted(f for f in os.listdir(os.path.join(CORPUS, 'nro')) if f.endswith('.nro'))
    same = 0
    for nro in nros:
        name = nro[:-4]
        path = os.path.join(CORPUS, 'nro', nro)
        run = subprocess.run([a.nxaot, 'image', path, a.work, name], capture_output=True, text=True)
        if run.returncode:
            failures.append(f'nxaot image {nro}: {run.stdout}{run.stderr}')
            continue
        m = manifest(a.nxaot, os.path.join(a.work, name + '.aot.tsv'))
        if m.returncode or m.stdout != open(os.path.join(a.work, name + '.aot.tsv')).read():
            failures.append(f'{name}.aot.tsv, as nxaot image writes it, does not round-trip')
        from_nro = discover_rows(a.nxaot, path, os.path.join(a.work, name + '.nro.tsv'))
        from_image = discover_rows(a.nxaot, os.path.join(a.work, name + '.aot.tsv'), os.path.join(a.work, name + '.image.tsv'))
        if from_nro != from_image:
            diff = next((f'{x!r} / {y!r}' for x, y in zip(from_nro, from_image) if x != y), 'lengths differ')
            failures.append(f'{nro}: discovery of the behaviour image differs from the NRO\'s: {diff}')
        else:
            same += 1
    print(f'behaviour images: {same} of {len(nros)} corpus NROs give the same discovery as their image.bin and manifest')
    for line in failures:
        print('FAIL ' + line)
    print(f'manifest test: {len(failures)} failures')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
