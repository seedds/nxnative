#!/usr/bin/env python3
"""Compare CPU shader rewrites with a git baseline; no translation, Metal or game files.

Run from a checkout containing the candidate:
  python3 scripts/bench_shader_scans.py --baseline <base-commit> --output results.json
Only dmsh_rewrite and dmsh_fetch come from the baseline; shared dependencies and
public test fixtures come from this checkout. Use the parent of the scan changes.
"""
import argparse
import hashlib
import importlib
import json
from pathlib import Path
import platform
import random
import statistics
import subprocess
import sys
import time
import types

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'dm/gfx/tools'), str(ROOT / 'dm/gfx/tests')]
import dmsh_rewrite as rewrite  # noqa: E402
import dmsh_fetch as fetch  # noqa: E402
import dmsh_reflect as reflect  # noqa: E402
import dmsh_wide as wide  # noqa: E402


def baseline_module(name, revision):
    path = 'dm/gfx/tools/' + name + '.py'
    source = subprocess.check_output(['git', 'show', revision + ':' + path], cwd=ROOT, text=True)
    module = types.ModuleType('baseline_' + name)
    module.__file__ = str(ROOT / path)
    exec(compile(source, module.__file__, 'exec'), vars(module))
    return module


def passes(r, f):
    # Fragment rewrite order, excluding reflection, extra entries, compilation and I/O.
    return [r.per_vertex, r.precise, r.bit0_mask, f.select_guards, r.mul_extended,
            r.float_add_msl, r.gather_offsets, r.hmma_msl, r.shared_store_msl,
            reflect.pool_arrays, wide.wide_loads, r.tg_fence, r.border_emu,
            r.frag_z_snap, r.uniform_tests, r.blend_zero]


def pipeline(source, functions):
    metadata = []
    for function in functions:
        result = function(source)
        if isinstance(result, tuple):
            source = result[0]
            metadata.append(result[1:])
        else: source = result
    return source, metadata


def fixtures():
    bit = importlib.import_module('test_dmsh_bit0_mask')
    uniform = importlib.import_module('test_dmsh_uniform_tests')
    gather = importlib.import_module('test_dmsh_gather_offsets')
    fence = importlib.import_module('test_dmsh_tg_fence')
    border = importlib.import_module('test_dmsh_border')
    yield 'bit0-small', bit.reduced(100)
    yield 'bit0-large', bit.reduced(3200)
    yield 'uniform', uniform.SHADER
    lines = ''.join(f'    bool _{900 + i} = as_type<int>({uniform.WORD}.w) != 100;\n' for i in range(3200))
    yield 'uniform-dense', uniform.SHADER.replace('    main0_out out = {};\n', '    main0_out out = {};\n' + lines)
    yield 'gather', gather.HEAD + gather.KERNEL
    yield 'fence', fence.SCAN % 64
    yield 'border', border.GRASS
    # Adversarial prefix hits as well as successful casts; these are synthetic text.
    for label, token in [('bare-prefix', 'as_type<'), ('wrong-type', 'as_type<float>(x);\n')]:
        yield label, 'using namespace metal;\n// spvDescriptorSet0\n' + token * 16000


def timed(function, source, loops):
    start = time.process_time_ns()
    for _ in range(loops): function(source)
    return (time.process_time_ns() - start) / 1e6


def compare(label, source, functions, repeats, rng):
    results = [function(source) for function in functions.values()]
    if any(result != results[0] for result in results[1:]): raise AssertionError('different outputs: ' + label)
    loops, samples = {}, {key: [] for key in functions}
    for key, function in functions.items():
        n = 1
        while timed(function, source, n) < 25 and n < 4096: n *= 2
        loops[key] = n
    for _ in range(repeats):
        order = list(functions)
        rng.shuffle(order)
        for key in order: samples[key].append(timed(functions[key], source, loops[key]) / loops[key])
    medians = {key: statistics.median(values) for key, values in samples.items()}
    print(label + ': ' + '; '.join(f'{key} {value:.4f} ms' for key, value in medians.items()), flush=True)
    return {'case': label, 'characters': len(source), 'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
            'output_equal': True, 'cpu_ms': medians, 'samples_cpu_ms': samples, 'loops': loops,
            'speedup': medians['baseline'] / medians['candidate']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', required=True)
    parser.add_argument('--repeats', type=int, default=11)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.repeats < 3: parser.error('repeats must be at least 3')
    revision = subprocess.check_output(['git', 'rev-parse', args.baseline + '^{commit}'], cwd=ROOT, text=True).strip()
    old_r, old_f = (baseline_module(name, revision) for name in ('dmsh_rewrite', 'dmsh_fetch'))
    old_passes, new_passes = passes(old_r, old_f), passes(rewrite, fetch)
    rng, rows = random.Random(1932), []
    for label, source in fixtures():
        rows.append(compare('bundle/' + label, source, {'baseline': lambda s: pipeline(s, old_passes),
                                                       'candidate': lambda s: pipeline(s, new_passes)}, args.repeats, rng))
    # Isolated pass timings on the same raw fixture, not the bundle's intermediate sources.
    large = dict(fixtures())['bit0-large']
    for name in ('uniform_tests', 'select_guards', 'gather_offsets', 'tg_fence'):
        old, new = (old_f, fetch) if name == 'select_guards' else (old_r, rewrite)
        rows.append(compare('pass/' + name, large, {'baseline': getattr(old, name), 'candidate': getattr(new, name)}, args.repeats, rng))
    report = {'scope': __doc__, 'baseline': revision, 'python': sys.version, 'platform': platform.platform(),
              'checkout_revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'benchmark_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'timer': 'process_time_ns; randomized interleaving, warm calibration >=25ms per sample',
              'repeats': args.repeats, 'candidate_sha256': {name: hashlib.sha256((ROOT / 'dm/gfx/tools' / name).read_bytes()).hexdigest()
                                                         for name in ('dmsh_rewrite.py', 'dmsh_fetch.py')}, 'results': rows}
    if args.output: args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__': main()
