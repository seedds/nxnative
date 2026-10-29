#!/usr/bin/env python3
"""The census test (CTest `census`): `nxaot census` over gen_vectors.py's synthetic code blob must find the counts the generator wrote for it.

  test_census.py --nxaot <path to nxaot> --work <scratch folder>

No game module is involved: the blob is made of hand-encoded instructions (gen_vectors.py, census_blob). Exit 0 when every expected count matches.
"""

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = 0x10008000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nxaot', required=True)
    ap.add_argument('--work', required=True)
    args = ap.parse_args()
    os.makedirs(args.work, exist_ok=True)
    blob = os.path.join(args.work, 'census_blob.bin')
    expect_path = os.path.join(args.work, 'census_expect.tsv')
    subprocess.run([sys.executable, os.path.join(HERE, 'gen_vectors.py'), '--census-blob', blob, '--census-expect', expect_path], check=True)
    run = subprocess.run([args.nxaot, 'census', blob, '--base', f'{BASE:x}'], check=True, capture_output=True, text=True)
    with open(os.path.join(args.work, 'census_found.tsv'), 'w', newline='\n') as f:
        f.write(run.stdout)
    found = {}
    for line in run.stdout.splitlines():
        key, _, count = line.partition('\t')
        found[key] = int(count)
    expect = {}
    with open(expect_path) as f:
        for line in f:
            key, _, count = line.rstrip('\n').partition('\t')
            expect[key] = int(count)
    bad = [(k, v, found.get(k)) for k, v in sorted(expect.items()) if found.get(k) != v]
    for k, want, got in bad:
        print(f'MISMATCH {k}: expected {want}, census found {got}')
    print(f'census test: {len(expect)} expected counts, {len(bad)} mismatches ({len(found)} keys reported; blob {os.path.getsize(blob)} bytes)')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
