#!/usr/bin/env python3
"""Opt-in RG11B10Float attachment diagnostic. Requires a new output directory.

The input set contains zero and positive finite FP32 values below the largest
finite code in both formats. Comparisons do not prescribe GPU rounding.
results.json is published only after a complete measurement; failure.json
records an unsuccessful attempt. Words in binary artifacts are little-endian.
"""
import argparse
import bisect
import collections
import hashlib
import json
import os
import platform
import struct
import subprocess
import sys
import uuid
from datetime import UTC, datetime
from fractions import Fraction
from pathlib import Path
from typing import Any

SOURCE = Path(__file__).resolve().with_name('t_metal_conversion_probe.m')
INPUT_VERSION = 'original-95-v1'
INPUT_SHA256 = '90d94428e0a6ccb1b4e36a0e99f5238496daa04cb099c938ef4b8ad56151bbfb'


def value(code, m):
    e, f = divmod(code, 1 << m)
    return Fraction(f if e == 0 else (1 << m) + f) * Fraction(2) ** ((1 if e == 0 else e) - 15 - m)


def fpbits(x):
    return struct.unpack('<I', struct.pack('<f', float(x)))[0]


def input_cases():
    cases = {}
    for m in (5, 6):
        for lo in (0, 1, 2, (1 << m)-2, (1 << m)-1, 1 << m, (14 << m)-1, 14 << m, (15 << m)-1, 15 << m, (16 << m)-1, 16 << m, (20 << m)+7, (29 << m)-1):
            b = fpbits((value(lo, m) + value(lo+1, m)) / 2)
            for delta in (-1, 0, 1):
                cases.setdefault(b+delta, []).append({'boundary_mantissa_bits': m, 'lower_code': lo, 'position': delta})
    for x in (0, 2**-20, 2**-14, 0.1, 0.5, 1, 1.001, 2, 10, 100, 1000, 30000):
        cases.setdefault(fpbits(x), []).append({'anchor': str(x)})
    return dict(sorted(cases.items()))


def serialize(words):
    return b''.join(struct.pack('<I', word) for word in words)


def deserialize(data):
    if len(data) % 4:
        raise ValueError('word data is not a multiple of four bytes')
    return [x[0] for x in struct.iter_unpack('<I', data)]


def candidates(bits, m):
    x = Fraction.from_float(struct.unpack('<f', struct.pack('<I', bits))[0])
    table = [value(i, m) for i in range(31 * (1 << m))]
    lo = bisect.bisect_right(table, x) - 1
    if lo < 0 or lo >= len(table)-1:
        raise ValueError('input outside nonnegative finite bounded domain')
    dl, dh = x-table[lo], table[lo+1]-x
    return lo, lo if dl < dh or (dl == dh and lo % 2 == 0) else lo+1, dl == dh


def analyze(cases, packed):
    if len(cases) != len(packed):
        raise ValueError('incomplete packed output')
    rows = []
    for (bits, origins), word in zip(cases.items(), packed):
        channels = []
        for name, m, shift in (('R', 6, 0), ('G', 6, 11), ('B', 5, 22)):
            lo, near, tie = candidates(bits, m)
            observed = (word >> shift) & ((1 << (m+5))-1)
            relation = 'both' if observed == lo == near else 'nearest_only' if observed == near else 'lower_only' if observed == lo else 'other'
            channels.append({'channel': name, 'lower': lo, 'nearest_even': near, 'observed': observed, 'relation': relation, 'tie': tie})
        rows.append({'input_fp32_bits': f'{bits:08x}', 'packed_hex': f'{word:08x}', 'origins': origins, 'channels': channels})
    return rows


def validate_report(report, run_id, cases, packed):
    if report.get('status') != 'completed' or report.get('run_id') != run_id or report.get('case_count') != len(cases):
        raise ValueError('wrong run or incomplete report')
    if report.get('cases') != analyze(cases, packed):
        raise ValueError('report observations do not match complete raw output')


def command(*args):
    return subprocess.run(args, check=True, text=True, capture_output=True).stdout.strip()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', required=True, type=Path, help='new directory; must not already exist')
    args = parser.parse_args()
    try:
        args.output_dir.mkdir(parents=False, exist_ok=False)
    except OSError as e:
        parser.exit(2, f'cannot create fresh output directory: {e}\n')
    out = args.output_dir.resolve()
    run_id = str(uuid.uuid4())
    try:
        cases = input_cases()
        inputs = serialize(cases)
        if len(cases) != 95 or sha(inputs) != INPUT_SHA256:
            raise ValueError('input set identity mismatch')
        (out/'inputs.bin').write_bytes(inputs)
        compiler = command('xcrun', '--find', 'clang')
        metadata: dict[str, Any] = {'run_id': run_id, 'started_utc': datetime.now(UTC).isoformat(),
                    'os': command('sw_vers'), 'hardware': command('sysctl', '-n', 'machdep.cpu.brand_string'),
                    'architecture': platform.machine(), 'clang': command(compiler, '--version'),
                    'sdk_path': command('xcrun', '--sdk', 'macosx', '--show-sdk-path'),
                    'sdk_version': command('xcrun', '--sdk', 'macosx', '--show-sdk-version'),
                    'input_version': INPUT_VERSION, 'input_sha256': sha(inputs),
                    'python_source_sha256': sha(Path(__file__).read_bytes()), 'metal_source_sha256': sha(SOURCE.read_bytes()),
                    'format': 'MTLPixelFormatRG11B10Float', 'path': 'fragment output -> render attachment store -> blit',
                    'shader_compile_options': 'Metal default (nil)', 'domain': 'nonnegative finite FP32 below largest finite code in each channel'}
        compile_args = [compiler, '-isysroot', metadata['sdk_path'], '-fobjc-arc', '-framework', 'Foundation', '-framework', 'Metal', str(SOURCE), '-o', str(out/'probe')]
        metadata['compile_command'] = compile_args
        built = subprocess.run(compile_args, capture_output=True, text=True, check=True)
        (out/'compile.txt').write_text(built.stdout + built.stderr)
        run = subprocess.run([str(out/'probe'), str(out/'inputs.bin'), str(out/'packed.bin')], capture_output=True, text=True, check=True)
        (out/'execution.txt').write_text('stdout:\n' + run.stdout + '\nstderr:\n' + run.stderr)
        metadata['gpu'] = run.stdout.strip()
        raw = (out/'packed.bin').read_bytes()
        packed = deserialize(raw)
        rows = analyze(cases, packed)
        counts = {name: dict(collections.Counter(row['channels'][i]['relation'] for row in rows)) for i, name in enumerate(('R', 'G', 'B'))}
        report = {'status': 'completed', 'run_id': run_id, 'metadata': metadata, 'case_count': len(rows), 'packed_sha256': sha(raw), 'counts': counts, 'cases': rows}
        validate_report(report, run_id, cases, packed)
        temp = out/'results.json.tmp'
        temp.write_text(json.dumps(report, indent=2)+'\n')
        os.replace(temp, out/'results.json')
        print(json.dumps({'run_id': run_id, 'case_count': len(rows), 'gpu': metadata['gpu'], 'counts': counts}, indent=2))
    except (OSError, ValueError, subprocess.SubprocessError) as e:
        details: dict[str, Any] = {'status': 'failed', 'run_id': run_id, 'error': str(e)}
        if isinstance(e, subprocess.CalledProcessError):
            details.update(command=e.cmd, returncode=e.returncode, stdout=e.stdout, stderr=e.stderr)
        (out/'failure.json').write_text(json.dumps(details, indent=2)+'\n')
        print(f'measurement failed: {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
