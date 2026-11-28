#!/usr/bin/env python3
"""Rewrite Atmosphère mod code plugins (exefs/subsdk<N>) once into <mod>/exefs/nxnative/.

The Mac host loads prepared plugins from <mod>/exefs/nxnative/ whenever the mod's RomFS folder is
listed in DM_ROMFS_OVERLAY (dm/host/hle_exl.c). Run this script again after updating the rewriter.

Usage:
    python3 scripts/prepare_mod.py <mod folder> [<mod folder> ...]
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

REPO = Path(__file__).resolve().parents[1]
PATCH_NSO = REPO / 'dm/tools/patch_nso.py'
SUBSDK_RE = re.compile(r'^subsdk([0-9]+)$')


def find_plugins(exefs: Path) -> list[Path]:
    """Return regular subsdk<N> files under exefs in numeric order."""
    found: list[tuple[int, str, Path]] = []
    for entry in exefs.iterdir():
        m = SUBSDK_RE.match(entry.name)
        if not m or not entry.is_file():
            continue
        if len(entry.name) >= 16:
            raise ValueError(f'{entry}: module name is too long ({len(entry.name)} >= 16)')
        found.append((int(m.group(1)), entry.name, entry))
    found.sort()
    return [p for _, _, p in found]


def prepare_mod(mod_dir: Path, python: str = sys.executable) -> int:
    """Rewrite every subsdk<N> NSO in <mod_dir>/exefs into <mod_dir>/exefs/nxnative."""
    exefs = mod_dir / 'exefs'
    if not exefs.is_dir():
        raise FileNotFoundError(f'{mod_dir} has no exefs folder')
    if (exefs / 'main').is_file():
        print(f'[RED] {exefs / "main"} replaces the game binary, which is not supported: skipping main', file=sys.stderr)
    plugins = find_plugins(exefs)
    if not plugins:
        raise FileNotFoundError(f'{exefs} has no subsdk<N> plugin')

    for plugin in plugins:
        with plugin.open('rb') as f:
            magic = f.read(4)
        if magic != b'NSO0':
            raise ValueError(f'{plugin} is not an NSO binary (magic {magic!r})')

    out = exefs / 'nxnative'
    tmp = exefs / 'nxnative.tmp'
    if tmp.exists():
        shutil.rmtree(tmp)
    tmp.mkdir(parents=True)

    # Clear DM_GAME so a game's MATCH_CUTS / build rows never attach to a mod's subsdk plugin.
    env = {k: v for k, v in os.environ.items() if k != 'DM_GAME'}
    try:
        for plugin in plugins:
            subprocess.run([python, str(PATCH_NSO), str(plugin), str(tmp), plugin.name], env=env, check=True)
    except Exception:
        shutil.rmtree(tmp, ignore_errors=True)
        raise

    if out.exists():
        shutil.rmtree(out)
    tmp.rename(out)
    print(f'prepared {len(plugins)} plugin(s) in {out}')
    return len(plugins)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description='Rewrite Atmosphère mod exefs/subsdk<N> plugins for nxnative.')
    parser.add_argument('mods', nargs='+', type=Path, help='Mod folder(s) containing exefs/ (and optionally romfs/)')
    args = parser.parse_args(argv)
    for mod in args.mods:
        try:
            prepare_mod(mod)
        except (FileNotFoundError, ValueError, subprocess.CalledProcessError) as exc:
            print(f'[RED] {exc}', file=sys.stderr)
            return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
