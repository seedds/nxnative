#!/usr/bin/env python3
# Game files already extracted (a console dump: folders, not NSPs) → the layout build_ipa_from_nsp.py extracts NSPs into. build_ipa_from_nsp.py calls this when it is given such a folder, and
# goes on with its other steps; run on its own, this script does only this step.
# The dump folder holds:
#   base/exefs/ (main, main.npdm, sdk, rtld, subsdk*), base/romfs/   (base/ is optional when an update is there: its ExeFS and RomFS are whole, and nothing packs the base's for it;
#                                                                  base/romfs alone may be left out too)
#   updates/<version>/exefs/, updates/<version>/romfs/   (optional with base/; <version> is the title version as a decimal number, 458752 = 0x70000)
#   dlc/<DLC title ID>/<n>/romfs/   (optional; the DLC title ID in hex), or dlc/<DLC title ID>/<n>/base/romfs/ and dlc/<DLC title ID>/<n>/updates/<version>/romfs/ (a DLC's update,
#                                   whole like a game's: the newest one is packed, else the base)
# Each of those version folders may instead hold one folder per program of the application, program/<index>/exefs/ and program/<index>/romfs/ (DLC: program/0/romfs/). A Nintendo
# Switch 2 Edition's update has the Switch 1 game as program 0 and the Switch 2 one as program 1 (Xenoblade Chronicles 2 1114112: 0100E95004038000 and 0400E95004038001): the Switch 2
# program is packed when there is one, a version without it packs its program 0, and --program N picks program N (a version without it is left out). A multi-program application whose
# Each program is packed with its own RomFS, as Horizon mounts it: a Switch 2 Edition's Switch 2 program may have only a placeholder there (Xenoblade Chronicles 2 1114112: one
# 5-byte dummy.bin) and find its data elsewhere (Xenoblade Chronicles 2: in a DLC, dm/games/0400E95004038001/README.md); program 0's data would be the Switch 1 game's.
# A multi-program application whose programs are all Switch 1 or all Switch 2 ones (Super Mario 3D All-Stars: a launcher and one program per game) packs program 0 unless --program names another.
# An update's RomFS in such a dump is the patched RomFS (base + the update's BKTR patch, the tree the game sees), not a patch: it is packed on its own, never laid over the base
# (Donkey Kong Bananza 458752 drops 42 base files, among them the Product.100 tables its Product.200 ones replace).
# Writes, under the build folder (<dump folder>/<title ID>_build unless --work-dir names another):
#   extract/v<version>/ExeFS/   the newest version's code modules (APFS clones, or copies on another volume)
#   extract/v<version>/RomFS.img   its RomFS as one image (dm/tools/romfs_image.py: the host's DM_ROMFS, the IPA's romfs.img)
#   aoc/<index>.img   each DLC's RomFS (index = DLC title ID − base title ID − 0x1000), the host's DM_AOC_DIR
# A dump has no control data, so there is no control.nacp or icon.jpg beside them, as there is after nx_extract.
# The title ID is the program ID in program 0's main.npdm (the base's, else the newest update's). A dump that holds only a Switch 2 Edition's own program, without program/ folders, names
# a program that is not a base title ID (Xenoblade Chronicles 2: 0400E95004038001 for 0100E95004038000): the title ID then comes from the DLC (each DLC's title ID is the game's + 0x1000 + its index), or from --title-id. A repeat run keeps code modules that are the same and an image whose source tree, packer and image are unchanged (romfs_image.py --reuse).
# Usage: pack_dump.py <dump folder> [--version N] [--program N] [--title-id ID] [--work-dir DIR]   (exit 1 on error)
import argparse, filecmp, shutil, struct, subprocess, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'dm/tools'))
import romfs_image


class Error(Exception): pass


def program_id(npdm):   # the ACI0 program ID of main.npdm
    d = npdm.read_bytes()
    if d[:4] != b'META': raise Error(f'{npdm}: not an NPDM')
    off, size = struct.unpack_from('<II', d, 0x70)
    if d[off:off + 4] != b'ACI0' or size < 0x18: raise Error(f'{npdm}: no ACI0 section')
    return struct.unpack_from('<Q', d, off + 0x10)[0]


def clone(src, dst):   # an APFS clone on the same volume, else a copy
    if subprocess.run(['cp', '-c', src, dst], capture_output=True).returncode != 0: shutil.copy2(src, dst)


def exefs(src, dst):
    files = sorted(f for f in src.iterdir() if f.is_file() and not f.name.startswith('.'))
    if dst.is_dir() and {f.name for f in files} == {f.name for f in dst.iterdir()} and all(filecmp.cmp(f, dst / f.name, shallow=False) for f in files): return
    tmp = dst.with_name(dst.name + '.partial')
    shutil.rmtree(tmp, ignore_errors=True); tmp.mkdir(parents=True)
    for f in files: clone(f, tmp / f.name)
    shutil.rmtree(dst, ignore_errors=True); tmp.rename(dst)


def pack(src, img, say=print):
    img.parent.mkdir(parents=True, exist_ok=True)
    say(f'packing {src} → {img}')
    try: n = romfs_image.pack(src, img, reuse=True)
    except RuntimeError as e: raise Error(f'{src}: {e}') from e   # the files changed while packing
    say(f'  {n:,} entries, {img.stat().st_size / 2**30:.2f} GB')


def numbered(folder):   # {number: subfolder} for the subfolders named by a decimal number
    return {int(p.name): p for p in folder.iterdir() if p.is_dir() and p.name.isdigit()} if folder.is_dir() else {}


def pid_of(folder):   # the program ID folder/exefs/main.npdm names; None without one
    try: return program_id(folder / 'exefs/main.npdm') if (folder / 'exefs/main.npdm').is_file() else None
    except Error: return None


def switch2(pid): return pid is not None and pid >> 56 == 0x04   # Switch 2 title IDs start 04, Switch 1 ones 01


def program_dir(folder, index=None):
    """The folder of one version (base/, updates/<version>/) that holds the program's exefs/ and romfs/: folder itself, or one of its program/<index>/ folders: index, else
    the Switch 2 program of a Switch 2 Edition (a program whose ID starts 04 beside a program 0 whose ID starts 01), else program 0. None when index names a program the folder lacks"""
    progs = numbered(folder / 'program')
    if not progs: return folder if index in (None, 0) or not (folder / 'exefs').is_dir() else None   # a folder without program/ is taken for the one program given, when it has code
    if index is not None: return progs.get(index)
    first = min(progs)
    if not switch2(pid_of(progs[first])): return next((progs[i] for i in sorted(progs) if i != first and switch2(pid_of(progs[i]))), progs[first])
    return progs[first]


def app_dir(folder):   # the folder of the application's own program (program 0) in a version folder, which names its title ID
    progs = numbered(folder / 'program')
    return progs[min(progs)] if progs else folder


def updates(dump, program=None):   # {update version: its program's folder}; with program, only the updates that hold it
    return {v: d for v, p in numbered(dump / 'updates').items() if (d := program_dir(p, program)) is not None}


def base_dir(dump, program=None):   # base/'s program folder when it is a whole version (code and RomFS), which can be packed on its own; else None
    d = program_dir(dump / 'base', program)
    return d if d is not None and (d / 'exefs/main.npdm').is_file() and (d / 'romfs').is_dir() else None


def has_base(dump, program=None): return base_dir(dump, program) is not None


def npdm_of(dump):   # the main.npdm that names the game: program 0's of the base, else of the newest update that has one; None when there is none
    for p in [dump / 'base'] + [p for _, p in sorted(numbered(dump / 'updates').items(), reverse=True)]:
        if (app_dir(p) / 'exefs/main.npdm').is_file(): return app_dir(p) / 'exefs/main.npdm'
    return None


def is_dump(folder):   # whether a folder has the layout above
    return npdm_of(folder) is not None and any((program_dir(p) / 'romfs').is_dir() for p in [folder / 'base', *numbered(folder / 'updates').values()])


def romfs_of(folder):   # a DLC version folder's RomFS: romfs/, or program 0's
    return next((r for r in (folder / 'romfs', app_dir(folder) / 'romfs') if r.is_dir()), None)


def dlc_romfs(d):   # a DLC's RomFS folder in dlc/<title ID>/: <n>/romfs, romfs, or the newest of <n>/updates/<version>/romfs and <n>/base/romfs (each also under program/0/)
    found = []
    for n in sorted(p for p in d.iterdir() if p.is_dir() and p.name not in ('romfs', 'program')):
        if (r := romfs_of(n)): found.append(r); continue
        ups = {v: r for v, p in numbered(n / 'updates').items() if (r := romfs_of(p))}
        if ups: found.append(ups[max(ups)])
        elif (r := romfs_of(n / 'base')): found.append(r)
    if not found and (r := romfs_of(d)): found = [r]
    if len(found) != 1: raise Error(f'{d}: expected one <n>/romfs (or <n>/base/romfs, <n>/updates/<version>/romfs) folder, found {len(found)}')
    return found[0]


def code_program(dump):   # the program ID program 0's main.npdm names (the base's, else the newest update's)
    if (npdm := npdm_of(dump)) is None: raise Error(f'{dump}: no main.npdm in base/exefs or updates/<version>/exefs')
    return program_id(npdm)


def of_app(pid, app):   # whether program ID pid is one of application app's programs: app itself, a program index of it, or its Switch 2 Edition program (04… beside 01…)
    return pid & 0x00FFFFFFFFFFF000 == app & 0x00FFFFFFFFFFF000 and pid & 0xFFF < 0x10


def scan(dump, want=None, program=None):
    """What a dump folder holds: (title ID, {update version: its folder}, {DLC title ID: its RomFS folder}); has_base says whether base/ can be packed too.
    The title ID is program 0's program ID when that is a base title ID; else the game the DLC belong to, else want (the title ID the caller was given). program: as program_dir"""
    if not is_dump(dump): raise Error(f'{dump}: no main.npdm in base/exefs or updates/<version>/exefs, or no RomFS in base/ or updates/<version>/')
    pid = code_program(dump)
    vers = updates(dump, program)
    if program is not None and not vers and not has_base(dump, program): raise Error(f'{dump}: no program/{program}/ in base/ or updates/<version>/')
    dirs = {}
    for d in sorted(p for p in (dump / 'dlc').iterdir() if p.is_dir()) if (dump / 'dlc').is_dir() else []:
        try: dirs[int(d.name, 16)] = d
        except ValueError: raise Error(f'{d}: not a DLC title ID') from None
    app = pid
    if pid & 0xFFF:   # not a base title ID: a Switch 2 Edition's program, dumped without program/ folders
        apps = {(tid & ~0xFFF) - 0x1000 for tid in dirs}
        if len(apps) == 1: app = apps.pop()
        elif want is not None: app = want
        else: raise Error(f'{npdm_of(dump)}: program ID {pid:016X} is not a base title ID, and ' + ('the DLC belong to more than one game' if apps else 'no DLC names the game') + ': give its title ID (--title-id)')
        if app & 0xFFF: raise Error(f'{dump}: {app:016X} is not a base title ID')
    dlc = {}
    for tid, d in dirs.items():
        if not 0 < tid - app - 0x1000 <= 2000: raise Error(f'{d}: {tid:016X} is not a DLC of {app:016X}')
        dlc[tid] = dlc_romfs(d)
    return app, vers, dlc


def pack_version(src, app, out, say=print):   # one program's folder (base_dir, a value of updates) → out/ExeFS and out/RomFS.img
    if not (src / 'exefs/main').is_file() or not (src / 'exefs/main.npdm').is_file() or not (src / 'romfs').is_dir(): raise Error(f'{src}: no exefs/main, exefs/main.npdm and romfs')
    if not of_app(pid := program_id(src / 'exefs/main.npdm'), app): raise Error(f'{src}: program ID {pid:016X}, the base is {app:016X}')
    exefs(src / 'exefs', out / 'ExeFS')
    pack(src / 'romfs', out / 'RomFS.img', say)


def pack_dlc(app, dlc, aoc, say=print):   # {DLC title ID: its RomFS folder} → aoc/<index>.img; images of DLC no longer in the folder go
    want = {f'{tid - app - 0x1000}.img': roms for tid, roms in dlc.items()}
    if aoc.is_dir():
        for f in aoc.iterdir():
            if f.name.removesuffix('.inputs.json') not in want: f.unlink()
    for name, roms in want.items(): pack(roms, aoc / name, say)


def main():
    ap = argparse.ArgumentParser(description='Pack a dumped game (base/, updates/<version>/, dlc/) into the extraction layout of build_ipa_from_nsp.py')
    ap.add_argument('dump', type=Path, help='the dump folder (holds base/ or updates/<version>/, each with exefs/ and romfs/)')
    ap.add_argument('--version', type=int, help='the update version to pack (default: the newest in updates/; 0 = the base game)')
    ap.add_argument('--work-dir', type=Path, help='the build folder (default: <dump>/<title ID>_build)')
    ap.add_argument('--program', type=int, help='the program to pack from program/<N>/ folders (default: a Switch 2 Edition\'s Switch 2 program, else program 0)')
    ap.add_argument('--title-id', help='the game\'s title ID, when the code names a program that is not one and no DLC names the game (a Switch 2 Edition update alone)')
    a = ap.parse_args()
    dump = a.dump.resolve()
    try:
        if a.title_id and not (len(a.title_id) == 16 and all(c in '0123456789abcdefABCDEF' for c in a.title_id)): raise Error(f'--title-id takes 16 hex digits: {a.title_id}')
        app, vers, dlc = scan(dump, int(a.title_id, 16) if a.title_id else None, a.program)
        ver = a.version if a.version is not None else max(vers, default=0)
        if ver and ver not in vers: raise Error(f'no updates/{ver} (have: {", ".join(map(str, sorted(vers))) or "none"})')
        if not ver and not has_base(dump, a.program): raise Error(f'{dump}: no base/exefs and base/romfs to pack the base game from')
        src = vers[ver] if ver else base_dir(dump, a.program)
        work = (a.work_dir or dump / f'{app:016X}_build').resolve()
        out = work / 'extract' / f'v{ver}'
        print(f'{app:016X} version {ver}: {src}', flush=True)
        say = lambda s: print(s, flush=True)
        pack_version(src, app, out, say)
        pack_dlc(app, dlc, work / 'aoc', say)
    except Error as e: sys.exit(f'[RED] {e}')
    print(f'done: {out}' + (f', {len(dlc)} DLC in {work / "aoc"}' if dlc else ''))


if __name__ == '__main__':
    main()
