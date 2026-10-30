#!/usr/bin/env python3
"""Rebuild the self-contained ARM64 NativeAOT translator with the pinned SDK.

Usage: build.py --work <NSP work folder>/translator-build [--dotnet /path/to/dotnet]
Keeps NuGet/build scratch files in --work, then refreshes the tool lock and header.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--work', type=Path, required=True)
    ap.add_argument('--dotnet', default='dotnet')
    a = ap.parse_args()
    here = Path(__file__).resolve().parent
    repo = here.parents[3]
    work = a.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    dotnet = shutil.which(a.dotnet)
    if not dotnet:
        ap.error(f'dotnet not found: {a.dotnet}')
    env = dict(os.environ, DOTNET_CLI_TELEMETRY_OPTOUT='1')
    env.setdefault('DOTNET_CLI_HOME', str(work / 'dotnet-home'))
    env.setdefault('NUGET_PACKAGES', str(work / 'nuget'))

    def run(cmd, **kw):
        return subprocess.run([str(x) for x in cmd], cwd=repo, env=env, check=True, **kw)

    version = run([dotnet, '--version'], capture_output=True, text=True).stdout.strip()
    expected = json.loads((repo / 'global.json').read_text())['sdk']['version']
    if version != expected:
        ap.error(f'expected SDK {expected}, got {version}')
    run([dotnet, 'publish', here / 'dmsh_xlate.csproj', '-c', 'Release',
         '-r', 'osx-arm64', '--self-contained', 'true', '-p:PublishAot=true', '--disable-build-servers',
         '--artifacts-path', work / 'artifacts', '-o', work / 'osx-arm64',
         '-m:1', '-p:BuildInParallel=false', '-p:UseSharedCompilation=false'])
    run(['lipo', '-verify_arch', 'arm64', work / 'osx-arm64/dmsh_xlate'])
    sys.path.insert(0, str(here.parent))
    os.environ.update(env)
    import dmsh_chain
    import tool_lock
    # An SDK rebuild must not silently change the pinned SPIRV-Cross or Metal tools.
    lock = dict(l.split('\t', 1) for l in Path(dmsh_chain.LOCK).read_text().splitlines() if '\t' in l and not l.startswith('#'))
    for key, value in tool_lock.tools_now().items():
        if key in ('spirv-cross', 'metal_macosx', 'metal_iphoneos') and lock.get(key) != value:
            ap.error(f'{key} differs from the lock: {value}')
    dest = here.parent / 'dmsh_xlate_bin'
    staged = work / 'staged'
    if staged.exists():
        shutil.rmtree(staged)
    staged.mkdir()
    shutil.copy2(work / 'osx-arm64/dmsh_xlate', staged / 'dmsh_xlate')
    embedded = run([staged / 'dmsh_xlate', '--source-sha'], capture_output=True, text=True).stdout.strip()
    if embedded != dmsh_chain.xlate_source_sha():
        ap.error(f'published source fingerprint differs: {embedded}')
    shutil.copy2(staged / 'dmsh_xlate', dest / 'dmsh_xlate')
    for target in dest.iterdir():
        if target.name == 'dmsh_xlate':
            continue
        if target.is_dir():
            shutil.rmtree(target)
        else:
            target.unlink()
    run([sys.executable, here.parent / 'tool_lock.py', '--update-lock'])
    run([sys.executable, here.parent / 'dmsh_chain.py', '--header'])
    run([sys.executable, here.parent / 'tool_lock.py', '--check-tools'])


if __name__ == '__main__':
    main()
