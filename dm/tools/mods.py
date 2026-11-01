"""Mods for a build (scripts/build_ipa_from_nsp.py --mods): finds the mods in the folders given, the way the host finds them at run time (dm/host/hle_mods.c),
and stages them for the app.

A mod is a folder with romfs/ (files that replace or add to the game's) and/or exefs/ (code patches, .pchtxt or .ips: exefs_mods.py). A folder given may be:
- a mod itself;
- a folder holding one of the trees downloads come in: atmosphere/contents/<title ID>/ (Atmosphère; its romfs/ and exefs/), atmosphere/exefs_patches/<mod>/ (Atmosphère's
  code patches, applied by build ID: each <mod> folder holds .ips files), contents/<title ID>/<mod>/ (Ryujinx) or <title ID>/<mod>/ (yuzu's load folder);
- an exefs_patches/<mod> folder of .ips / .pchtxt files;
- a folder of mods (each subfolder one of the above).
A mod's code patch files usually cover several versions of the game; the rewriter applies those whose build ID is a module's (patch_nso.py, DM_EXEFS_PATCHES), and a mod
with code patches none of which fit this build stops the build (it is for another version). Other files in exefs/ (replacement modules: main, subsdk*, main.npdm, which
exlaunch mods bring) are not supported and stop the build.
"""
import os
import shutil
import subprocess
from pathlib import Path

PATCH_EXT = ('.pchtxt', '.ips')


class Mod:
    def __init__(self, name, folder, exefs=None):
        self.name, self.dir = name, Path(folder)
        self.romfs = self.dir / 'romfs' if (self.dir / 'romfs').is_dir() and any(_files(self.dir / 'romfs')) else None
        ex = Path(exefs) if exefs else self.dir / 'exefs'
        files = sorted(_files(ex)) if ex.is_dir() else []
        self.patches = [p for p in files if p.suffix.lower() in PATCH_EXT]
        self.other = [p for p in files if p.suffix.lower() not in PATCH_EXT]

    def __repr__(self): return f'Mod({self.name!r}, {self.dir})'


def _files(d):
    for root, dirs, names in os.walk(d):
        dirs[:] = [x for x in dirs if not x.startswith('.')]
        for n in names:
            if not n.startswith('.'): yield Path(root) / n


def _child_ci(d, name):
    try: return next((p for p in Path(d).iterdir() if p.name.lower() == name.lower() and p.is_dir()), None)
    except OSError: return None


def _is_mod(d): return (Path(d) / 'romfs').is_dir() or (Path(d) / 'exefs').is_dir()


def _patch_folder(d): return Path(d).is_dir() and any(p.suffix.lower() in PATCH_EXT for p in Path(d).iterdir() if p.is_file())


def _subdirs(d): return sorted((p for p in Path(d).iterdir() if p.is_dir() and not p.name.startswith('.')), key=lambda p: p.name)


def _tree(name, base, tid):
    """the mods of the trees under base; [] when it has none"""
    out = []
    atm = _child_ci(base, 'atmosphere')
    t = _child_ci(_child_ci(atm, 'contents'), tid) if atm and _child_ci(atm, 'contents') else None
    if t and _is_mod(t): out.append(Mod(name, t))
    ep = _child_ci(atm, 'exefs_patches') if atm else _child_ci(base, 'exefs_patches')
    if ep: out += [Mod(s.name, s, exefs=s) for s in _subdirs(ep) if _patch_folder(s)]
    c = _child_ci(base, 'contents')
    for t in filter(None, (_child_ci(c, tid) if c else None, _child_ci(base, tid))):
        out += [Mod(name, t)] if _is_mod(t) else [Mod(s.name, s) for s in _subdirs(t) if _is_mod(s)]
    return out


def find(folder, tid, _depth=0):
    """the mods in a folder given to --mods (see the module's note), in name order within a folder of mods"""
    p = Path(folder).expanduser()
    if not p.is_dir(): raise FileNotFoundError(f'no mod folder {p}')
    if _is_mod(p): return [Mod(p.name, p)]
    if _patch_folder(p): return [Mod(p.name, p, exefs=p)]
    got = _tree(p.name, p, tid)
    if got or _depth: return got
    return [m for s in _subdirs(p) for m in find(s, tid, 1)]


def stage(mods, applied, out):
    """copies each mod's romfs/ to out/<name>/romfs (an APFS clone where it can) and writes out/<name>/dm_code.tsv when its code patches went into the build (applied: the
    patch files the rewriter applied, from its <module>.mods.tsv files); out is made again from scratch. Returns [(name, files, code patch files applied)]"""
    if out.exists(): shutil.rmtree(out)
    out.mkdir(parents=True)
    rows = []
    for m in mods:
        d = out / m.name; d.mkdir()
        nfiles = 0
        if m.romfs:
            if subprocess.run(['cp', '-c', '-R', str(m.romfs), str(d / 'romfs')], capture_output=True).returncode: shutil.copytree(m.romfs, d / 'romfs')
            nfiles = sum(1 for _ in _files(d / 'romfs'))
        if (m.dir / 'env.txt').is_file(): shutil.copyfile(m.dir / 'env.txt', d / 'env.txt')   # the host settings the mod needs while it is on (hle_mods.c dm_mods_env)
        mine = [p for p in m.patches if str(p) in applied]
        if mine: (d / 'dm_code.tsv').write_text('# code patches in the build\'s codemods copy of the modules (scripts/build_ipa_from_nsp.py --mods): on with the other mods that have them\n' + ''.join(f'{p.name}\n' for p in mine))
        rows.append((m.name, nfiles, len(mine)))
    return rows


def applied_files(patched):
    """the patch files the rewriter applied to any module in the patched folder (<module>.mods.tsv)"""
    out = set()
    for t in Path(patched).glob('*.mods.tsv'):
        for ln in t.read_text(encoding='utf-8').splitlines():
            if ln and not ln.startswith('#'): out.add(ln.split('\t')[3])
    return out
