#!/usr/bin/env python3
"""Expand native mesh prewarm coverage using a packaged game's BNSH programs.

The seed supplies observed render states; the game supplies exact task/mesh/pixel
associations. Only matching pixel output signatures are combined. This predicts
additional pipelines, not every possible render state. Requires macOS and Xcode.
Writes a new manifest, never game files or saves. Review the report before using it.
"""
import argparse
from collections import defaultdict
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def source_file(source):
    path = source.split('#', 1)[0]
    marker = '.sarc.zs/'
    return path.split(marker, 1)[0] + '.sarc.zs' if marker in path else path


def mesh_programs(found, key_of):
    groups = defaultdict(dict)
    for where, stage, control, code in found:
        if '#var' in where:
            groups[where][stage] = key_of(control, code)
    return {(s.get('ts', '-'), s['ms'], s.get('ps', '-'))
            for s in groups.values() if 'ms' in s}


def pixel_signature(reflection):
    raster = reflection.get('raster', {})
    if 'colors' not in raster or 'omap' not in raster:
        raise ValueError('pixel reflection has no explicit output signature')
    return tuple(raster['colors']), raster['omap']


def seed_pixel(row):
    parts = row.split('\t')
    if len(parts) != 3 or parts[0] != 'M':
        raise ValueError('invalid native mesh row')
    data = bytes.fromhex(parts[2])
    if len(data) < 52 or data[48] != 4 or data[49] != 1 or data[50] or data[51]:
        raise ValueError('expected a mesh/pixel row without a specialized fragment')
    field = data[24:48]
    if b'\0' not in field:
        raise ValueError('unterminated pixel key')
    key = field.split(b'\0', 1)[0].decode('ascii')
    if not key:
        raise ValueError('mesh row has no pixel shader')
    return key


def validate_program(program, reflection, chain):
    task, mesh, pixel = program
    m = reflection(mesh + '_b')
    ms = m.get('mesh_stage', {})
    if m.get('chain') != chain or ms.get('kind') != 'ms' or bool(ms.get('task')) != (task != '-'):
        raise ValueError('mesh reflection disagrees with the source program')
    if task != '-':
        t = reflection(task + '_b')
        ts = t.get('mesh_stage', {})
        if t.get('chain') != chain or ts.get('kind') != 'ts' or ts.get('payload') != ms.get('payload'):
            raise ValueError('task/mesh payload or chain mismatch')
    if pixel == '-':
        return None
    f = reflection(pixel + '_b')
    if f.get('chain') != chain or f.get('stage') != 'ps':
        raise ValueError('pixel stage or chain mismatch')
    return pixel_signature(f)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', required=True, help='base title ID; selects the shader reader profile')
    parser.add_argument('--app', type=Path, required=True, help='packaged macOS .app, read only')
    parser.add_argument('--seed', type=Path, action='append', required=True, help='observed M rows; may repeat')
    parser.add_argument('--output', type=Path, required=True, help='new combined prewarm manifest')
    parser.add_argument('--report', type=Path, required=True, help='coverage and skipped signatures as JSON')
    args = parser.parse_args()
    protected = {p.resolve() for p in args.seed}
    for output in (args.output, args.report):
        if output.resolve() in protected or output.resolve().is_relative_to(args.app.resolve()):
            parser.error('outputs must be outside the app and differ from the seed files')
    if args.output.resolve() == args.report.resolve():
        parser.error('manifest and report must have different paths')
    os.environ['DM_GAME'] = args.game
    sys.path.insert(0, str(ROOT / 'dm/gfx/tools'))
    import shader_split as split
    import romfs

    resources = args.app / 'Contents/Resources'
    images = sorted((resources / 'msl').glob('b-*.img'))
    if len(images) != 1:
        raise ValueError('expected exactly one shader image in the app')
    image = str(images[0])
    chain = images[0].stem.removeprefix('b-')
    reflections = {}

    def reflection(key):
        if key not in reflections:
            with romfs.open(image + '/' + key + '.json') as handle:
                reflections[key] = json.load(handle)
        return reflections[key]

    rows = list(dict.fromkeys(line for seed in args.seed for line in seed.read_text().splitlines()
                              if line and not line.startswith('#')))
    templates = defaultdict(list)
    for row in rows:
        if row.startswith('M\t'):
            templates[pixel_signature(reflection(seed_pixel(row)))].append(row)
    if not templates:
        raise ValueError('at least one observed native mesh state is required')
    smap = [line.split('\t') for line in (resources / 'dm/shader_map.tsv').read_text().splitlines()
            if line and not line.startswith('#')]
    sources = sorted({source_file(row[4]) for row in smap if len(row) >= 5 and row[2] in ('mesh', 'task')})
    split.ZDICTS = split.zs_dicts(str(resources / 'romfs.img'))
    programs = set()
    for source in sources:
        path = str(resources / 'romfs.img') + '/' + source.lstrip('/')
        data = split.unzs(path) if source.endswith('.zs') else romfs.open(path, 'rb').read()
        found = []
        split.scan_blob(data, source, found)
        programs.update(mesh_programs(found, split.skey))
        print(f'{source}: {len(programs)} distinct native mesh programs so far', flush=True)
    groups = defaultdict(list)
    for program in sorted(programs):
        signature = validate_program(program, reflection, chain)
        if signature is not None:
            groups[signature].append('\t'.join(k + '_b' if k != '-' else '-' for k in program))
    generated = set()
    report = {'chain': chain, 'programs': len(programs), 'groups': []}
    with tempfile.TemporaryDirectory(prefix='nxnative-mesh-seed-') as folder:
        temp = Path(folder)
        tool = temp / 'seed_mesh'
        subprocess.run(['xcrun', 'clang', '-O2', '-fobjc-arc', '-I' + str(ROOT / 'dm/gfx/nvn/include'),
                        str(ROOT / 'dm/gfx/tools/pso_mesh_seed.m'), '-framework', 'Foundation', '-o', str(tool)], check=True)
        for index, (signature, triplets) in enumerate(sorted(groups.items())):
            entry = {'signature': signature, 'programs': len(triplets), 'seed_rows': len(templates[signature])}
            if templates[signature]:
                seed, pairs, output = (temp / f'{index}.{suffix}' for suffix in ('seed', 'pairs', 'out'))
                seed.write_text('\n'.join(templates[signature]) + '\n')
                pairs.write_text('\n'.join(triplets) + '\n')
                subprocess.run([str(tool), str(seed), str(pairs), str(output)], check=True)
                extra = set(output.read_text().splitlines())
                generated.update(extra)
                entry['generated'] = len(extra)
            report['groups'].append(entry)
    combined = list(dict.fromkeys(rows + sorted(generated)))
    report.update(generated_rows=len(generated), total_rows=len(combined),
                  mesh_rows=sum(row.startswith('M\t') for row in combined))
    args.output.write_text('\n'.join(combined) + '\n')
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(f"Wrote {report['mesh_rows']} mesh rows; no app files or saves were changed.")


if __name__ == '__main__':
    main()
