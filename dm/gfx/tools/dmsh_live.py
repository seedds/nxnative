#!/usr/bin/env python3
"""Translate an NVN1 graphics stage submitted by the Mac host.

The request contains the control and code bytes from nvnProgramSetShaders.
Use the same translation and Metal compiler as the packer, with an isolated
profile and library so the packaged shader tables and libraries stay read-only.
"""
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
from tempfile import TemporaryDirectory

import dmsh_chain


def compile_stage(profile, cache, chain, request, key):
    import shader_split as split
    import shader_lib

    current = dmsh_chain.version()
    if chain != current:
        raise ValueError(f'Mac host expects shader chain {chain}, compiler is {current}')
    control = (request / 'control').read_bytes()
    code = (request / 'code').read_bytes()
    stage = split.glslc_stage(control, code)
    if stage not in ('vs', 'ps') or split.nvn2_ctl(control):
        raise ValueError('the runtime compiler currently accepts NVN1 vertex and pixel shaders')
    if reason := split.check(key, stage, control, code):
        raise ValueError(reason)
    if split.skey(control, code) != key:
        raise ValueError('shader bytes differ from the NVN program key')

    source_only = os.environ.get('DM_NVN_RUNTIME_SOURCE') == '1'
    cache = cache / ('source' if source_only else 'library') / chain / dmsh_chain.inputs(profile)
    cache.mkdir(parents=True, exist_ok=True)
    result = cache / key
    reflection = f'{key}_b.json'
    product_suffix = '.runtime.metal' if source_only else '.metallib'
    product = f'{key}_b{product_suffix}'
    if (result / reflection).is_file() and (result / product).is_file():
        return split.STAGE_NAME[stage], result

    with TemporaryDirectory(prefix='stage-', dir=cache) as temporary:
        work = Path(temporary)
        profile_dir = work / profile.id
        profile_dir.mkdir()
        shutil.copyfile(profile.table('game.tsv'), profile_dir / 'game.tsv')
        for name in ('shader_facts.tsv', 'bf_want.tsv', 'dual_want.tsv'):
            source = profile.table(name)
            if source.is_file():
                shutil.copyfile(source, profile_dir / name)

        name = f'{stage}_live.bin'
        (profile_dir / 'shader_map.tsv').write_text(f'{name}\t0\t{split.STAGE_NAME[stage]}\t{key}\t/live\n')
        (profile_dir / 'shader_map.tsv.conflicts.tsv').touch()
        (profile_dir / 'shader_pairs.tsv').touch()
        header = struct.pack('<8s8s24xII8x', b'BTCDBTCD', bytes.fromhex(key), len(control), len(code))
        (work / name).write_bytes(header + code + control)
        env = dict(os.environ, DM_GAME=str(profile_dir), DMSH_TARGETS='mac', J='1', DMSH_PROCS='0',
                   DMSH_SOURCE_ONLY='1' if source_only else '0')
        for option in ('IOSOUT', 'DMSH_CAS', 'DMSH_PREFILL', 'DMSH_PROGRESS'):
            env.pop(option, None)
        compiler = Path(__file__).with_name('dmsh_build.py')
        run = subprocess.run([sys.executable, '-B', compiler, work, work / 'libraries', key],
                             env=env, capture_output=True, text=True, timeout=120)
        library = shader_lib.view(work / 'libraries' / f'b-{chain}')
        report = json.loads((library / '_b_build.json').read_text()) if (library / '_b_build.json').is_file() else []
        if run.returncode or not report or report[0]['st'] != 'ok':
            reason = report[0]['err'] if report else (run.stderr or run.stdout).strip()[-800:]
            raise ValueError(reason or 'translation failed')
        ready = work / 'ready'
        ready.mkdir()
        for artifact in library.glob(key + '_*'):
            if artifact.name.endswith(('.json', product_suffix)):
                (ready / artifact.name).write_bytes(artifact.read_bytes())
        if not (ready / reflection).is_file() or not (ready / product).is_file():
            raise ValueError('translation did not produce a library and reflection')
        try:
            ready.rename(result)
        except OSError:
            if not (result / reflection).is_file() or not (result / product).is_file():
                raise
    return split.STAGE_NAME[stage], result


def main():
    profile, cache, chain, request, key = sys.argv[1:]
    profile = Path(profile).resolve()
    os.environ['DM_GAME'] = str(profile)
    try:
        stage, directory = compile_stage(dmsh_chain.load_game(), Path(cache), chain, Path(request), key)
        (Path(request) / 'result.json').write_text(json.dumps({'stage': stage, 'directory': str(directory)}))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        sys.exit(f'[RED] NVN runtime translation of {key}: {error}')


if __name__ == '__main__':
    main()
