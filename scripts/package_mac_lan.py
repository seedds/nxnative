#!/usr/bin/env python3
"""Package a built Mac host and its runtime shaders for the LAN launcher."""
import argparse
import os
from pathlib import Path
import tempfile
import zipfile

from run_mac_lan import build_layout


TABLES = ('shader_map.tsv', 'c1_full.tsv', 'cb_extent.tsv', 'shader_pairs.tsv', 'pso_pairs.tsv', 'pso_manifest.tsv')


def package(build, output, host=None, profile=None, shaders=None):
    title_id, _, romfs, shader_map = build_layout(build, profile)
    if not romfs.is_file():
        raise ValueError(f'missing build input: {romfs}')
    host = host or build / 'tools/dmhost'
    if not host.is_file() or not os.access(host, os.X_OK):
        raise ValueError(f'no executable Mac host at {host}; build the game from this checkout first')
    if shaders is None:
        libraries = sorted(path for path in (build / 'shader/cache').glob('b-*') if (path / 'inputs.tsv').is_file())
        if len(libraries) != 1:
            raise ValueError('expected one built shader chain; use --shaders to select the matching b-<chain> folder')
        shaders = libraries[0]
    if not shaders.name.startswith('b-') or not (shaders / 'inputs.tsv').is_file():
        raise ValueError('--shaders must name a completed b-<chain> library directory')
    libraries = sorted(path for path in shaders.iterdir() if path.is_file() and path.suffix in ('.metallib', '.json', '.tsv') and path.name != '_b_build.json')
    if not any(path.suffix == '.metallib' for path in libraries):
        raise ValueError(f'no compiled Mac shader libraries in {shaders}')
    scripts = Path(__file__).resolve().parent
    entries = [(host, 'dmhost'), (scripts / 'run_mac_lan.py', 'run_mac_lan.py'),
               (scripts / 'start_mac_lan.command', 'Start LAN.command')]
    entries += [(path, f'runtime/shaders/{shaders.name}/{path.name}') for path in libraries]
    entries += [(shader_map.parent / name, f'runtime/tables/{title_id}/{name}')
                for name in TABLES if (shader_map.parent / name).is_file()]
    output.parent.mkdir(parents=True, exist_ok=True)
    print(f'Packing {len(entries)} files for {title_id}, shader chain {shaders.name}', flush=True)
    # No ROM, keys, extracted modules, fonts, saves, preferences, logs or
    # GPU-specific archives are copied. Each player selects their own build.
    with tempfile.NamedTemporaryFile(dir=output.parent, prefix=output.name + '.', suffix='.partial', delete=False) as scratch:
        partial = Path(scratch.name)
    try:
        with zipfile.ZipFile(partial, 'w', zipfile.ZIP_DEFLATED) as archive:
            for index, (source, name) in enumerate(entries, 1):
                archive.write(source, f'lan-test-kit/{name}')
                if index % 5000 == 0:
                    print(f'Packed {index}/{len(entries)} files', flush=True)
        partial.replace(output)
    finally:
        partial.unlink(missing_ok=True)
    print(f'LAN kit: {output} ({output.stat().st_size / 1024**2:.1f} MiB)', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path, help='Completed nxnative Mac build from this checkout')
    parser.add_argument('--output', '-o', required=True, type=Path, help='Output ZIP, kept beside the game outside the source checkout')
    parser.add_argument('--host', type=Path, help='Override the build host with another matching Mac host')
    parser.add_argument('--profile', type=Path, help='Override the shader profile of a custom build folder')
    parser.add_argument('--shaders', type=Path, help='Matching b-<chain> shader library directory')
    args = parser.parse_args()
    try:
        package(*(path.expanduser().resolve() if path else None for path in (args.build, args.output, args.host, args.profile, args.shaders)))
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.exit(1, f'[RED] {error}\n')


if __name__ == '__main__':
    main()
