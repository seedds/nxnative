#!/usr/bin/env python3
# Game files already extracted (a console dump: folders, not NSPs) → the layout build_ipa_from_nsp.py extracts NSPs into. build_ipa_from_nsp.py calls this when it is given such a folder, and
# goes on with its other steps; run on its own, this script does only this step.
# The dump folder holds:
#   base/exefs/ (main, main.npdm, sdk, rtld, subsdk*), base/romfs/   (base/romfs is optional when an update has its RomFS: that one is whole, and nothing packs the base's for it)
#   updates/<version>/exefs/, updates/<version>/romfs/   (optional; <version> is the title version as a decimal number, 458752 = 0x70000)
#   dlc/<DLC title ID>/<n>/romfs/   (optional; the DLC title ID in hex)
#   A dump may have no base/ when it holds an update: the update's folder is the whole game (below), so the base game adds nothing to it (Fire Emblem Fortune's Weave: updates/131072 only).
#   exefs/ and romfs/ directly in the folder count as base/: one version of the game, as `nup extract` writes it (issue #1265).
# An update's RomFS in such a dump is the patched RomFS (base + the update's BKTR patch, the tree the game sees), not a patch: it is packed on its own, never laid over the base
# (Donkey Kong Bananza 458752 drops 42 base files, among them the Product.100 tables its Product.200 ones replace).
# Unless it is an update pack: updates/<version>/romfs_removed.txt beside its exefs/ and romfs/ says that romfs/ holds only the files the update adds or changes, and lists
# the base files the update drops (one RomFS path a line, e.g. Voice/USen/x.bwav; # starts a comment; an empty file drops none). The update's RomFS is then base/romfs with
# romfs/ laid over it and those paths taken out (romfs_image.py, layered_entries), and needs base/romfs. scripts/nup_update_pack.py writes one from a NUP, from the hashes the
# container stores, without unpacking the update's whole RomFS (Ocarina of Time v131072: 2,285 files, 350 MB, against a 26 GB tree).
# Writes, under the build folder (<dump folder>/<title ID>_build unless --work-dir names another):
#   extract/v<version>/ExeFS/   the newest version's code modules (APFS clones, or copies on another volume)
#   extract/v<version>/RomFS.img   its RomFS as one image (dm/tools/romfs_image.py: the host's DM_ROMFS, the IPA's romfs.img)
#   or, with --romfs-format folder, extract/v<version>/RomFS/   its RomFS as a folder of APFS clones of the dump's files (no space of its own on the dump's volume; a Mac app only)
#   aoc/<index>.img   each DLC's RomFS (index = DLC title ID − base title ID − 0x1000), the host's DM_AOC_DIR
# A dump has no control data, so there is no control.nacp or icon.jpg beside them, as there is after nx_extract.
# The title ID is the program ID in main.npdm. A repeat run keeps code modules that are the same and an image whose source tree, packer and image are unchanged (romfs_image.py --reuse).
# Usage: pack_dump.py <dump folder> [--version N] [--work-dir DIR] [--romfs-format image|folder]   (exit 1 on error)
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


def pack(src, img, say=print, removed=()):   # src: one RomFS folder, or a list of them laid over each other (the first wins; removed: paths the others lose)
    roots = src if isinstance(src, (list, tuple)) else [src]
    img.parent.mkdir(parents=True, exist_ok=True)
    say(f'packing {" over ".join(map(str, roots))} → {img}' + (f' ({len(removed):,} base files dropped)' if removed else ''))
    try: n = romfs_image.pack_layers(roots, img, removed, reuse=True)
    except RuntimeError as e: raise Error(f'{roots[0]}: {e}') from e   # the files changed while packing
    say(f'  {n:,} entries, {img.stat().st_size / 2**30:.2f} GB')


def clone_folder(roots, dst, say=print, removed=()):   # the RomFS as a folder of APFS clones (--romfs-format folder)
    say(f'cloning {" over ".join(map(str, roots))} → {dst}' + (f' ({len(removed):,} base files dropped)' if removed else ''))
    try: n, cloned, copied = romfs_image.clone_tree(roots, dst, removed, reuse=True)
    except (RuntimeError, OSError) as e: raise Error(f'{roots[0]}: {e}') from e
    say(f'  {n:,} entries' + (f', {cloned:,} files cloned' if cloned else '') + (f', {copied:,} copied (not on the same APFS volume: they take space)' if copied else '') + ('' if cloned or copied else ', reused (inputs unchanged)'))


def is_pack(src):   # an update pack: updates/<version>/ whose romfs/ holds only the update's own files, laid over the base's (romfs_removed.txt beside it)
    return (src / 'romfs_removed.txt').is_file()


def removed(src):   # the RomFS paths an update pack drops from the base
    try: return romfs_image.removed_paths((src / 'romfs_removed.txt').read_text(encoding='utf-8').splitlines())
    except (OSError, UnicodeDecodeError) as e: raise Error(f'{src / "romfs_removed.txt"}: {e}') from e


def has_code(src):   # base/ or updates/<version>/ with its code
    return (src / 'exefs/main.npdm').is_file()


def is_version(src):   # base/ or updates/<version>/ with its code and its RomFS (an update pack's RomFS is whole only with the base's under it)
    return has_code(src) and (src / 'romfs').is_dir() and not is_pack(src)


def updates(folder):   # {version: updates/<version>/}
    u = folder / 'updates'
    return {int(p.name): p for p in u.iterdir() if p.is_dir() and p.name.isdigit()} if u.is_dir() else {}


def base_of(folder):   # the base game's folder: base/, or the folder itself when it holds the version directly; None when it has neither
    if has_code(folder / 'base'): return folder / 'base'
    return folder if is_version(folder) else None


def is_dump(folder):   # whether a folder has the layout above: a base game's code with a RomFS beside it or in an update, or an update with its code and RomFS and no base
    base, ups = base_of(folder), updates(folder).values()
    if base: return (base / 'romfs').is_dir() or any((p / 'romfs').is_dir() and not is_pack(p) for p in ups)
    return any(is_version(p) for p in ups)


def scan(dump):
    """What a dump folder holds: (title ID, {update version: its folder}, {DLC title ID: its RomFS folder}). The title ID is the base game's program ID, else the newest whole update's"""
    if not is_dump(dump): raise Error(f'{dump}: no base/exefs/main.npdm with a RomFS in base/ or updates/<version>/, nor exefs/main.npdm and romfs in the folder or in updates/<version>/')
    vers = updates(dump)
    app = program_id((base_of(dump) or max((p for p in vers.values() if is_version(p)), key=lambda p: int(p.name))) / 'exefs/main.npdm')
    if app & 0xFFF: raise Error(f'{dump}: base program ID {app:016X} is not a base title ID')
    dlc = {}
    for d in sorted(p for p in (dump / 'dlc').iterdir() if p.is_dir()) if (dump / 'dlc').is_dir() else []:
        try: tid = int(d.name, 16)
        except ValueError: raise Error(f'{d}: not a DLC title ID') from None
        if not 0 < tid - app - 0x1000 <= 2000: raise Error(f'{d}: {tid:016X} is not a DLC of {app:016X}')
        roms = sorted(d.glob('*/romfs')) or ([d / 'romfs'] if (d / 'romfs').is_dir() else [])
        if len(roms) != 1: raise Error(f'{d}: expected one <n>/romfs folder, found {len(roms)}')
        dlc[tid] = roms[0]
    return app, vers, dlc


FORMATS = ('image', 'folder')


def pack_version(src, app, out, say=print, base=None, fmt='image'):
    """one version's folder (base/ or updates/<version>/) → out/ExeFS and out/RomFS.img (fmt 'folder': out/RomFS/, APFS clones). An update pack (is_pack) is laid over base/romfs,
    so base is the dump's base folder (base_of)"""
    if not (src / 'exefs/main').is_file() or not (src / 'exefs/main.npdm').is_file() or not (src / 'romfs').is_dir(): raise Error(f'{src}: no exefs/main, exefs/main.npdm and romfs')
    if (pid := program_id(src / 'exefs/main.npdm')) != app: raise Error(f'{src}: program ID {pid:016X}, the base is {app:016X}')
    if fmt not in FORMATS: raise Error(f'RomFS format {fmt!r}: one of {", ".join(FORMATS)}')
    roots, drop = [src / 'romfs'], set()
    if is_pack(src):
        if not base or not (base / 'romfs').is_dir(): raise Error(f'{src} is an update pack (romfs_removed.txt): it needs the base game\'s RomFS, base/romfs, to lay its files over')
        if base.resolve() == src.resolve(): raise Error(f'{src}: the base game cannot be an update pack')
        roots, drop = [src / 'romfs', base / 'romfs'], removed(src)
        say(f'{src.name}: an update pack, laid over {base / "romfs"}')
    exefs(src / 'exefs', out / 'ExeFS')
    img, folder = out / 'RomFS.img', out / 'RomFS'
    if fmt == 'folder':
        clone_folder(roots, folder, say, drop)
        for stale in (img, Path(str(img) + '.inputs.json')): stale.unlink(missing_ok=True)   # romfs.source() takes an image before a folder
    else:
        pack(roots, img, say, drop)
        if folder.exists(): shutil.rmtree(folder); Path(str(folder) + '.inputs.json').unlink(missing_ok=True)


def pack_dlc(app, dlc, aoc, say=print):   # {DLC title ID: its RomFS folder} → aoc/<index>.img; images of DLC no longer in the folder go
    want = {f'{tid - app - 0x1000}.img': roms for tid, roms in dlc.items()}
    if aoc.is_dir():
        for f in aoc.iterdir():
            if f.name.removesuffix('.inputs.json') not in want: f.unlink()
    for name, roms in want.items(): pack(roms, aoc / name, say)


def main():
    ap = argparse.ArgumentParser(description='Pack a dumped game (base/, updates/<version>/, dlc/) into the extraction layout of build_ipa_from_nsp.py')
    ap.add_argument('dump', type=Path, help='the dump folder (holds base/exefs with base/romfs or updates/<version>/romfs, or updates/<version>/ with exefs and romfs, or exefs and romfs directly)')
    ap.add_argument('--version', type=int, help='the update version to pack (default: the newest in updates/; 0 = the base game)')
    ap.add_argument('--work-dir', type=Path, help='the build folder (default: <dump>/<title ID>_build)')
    ap.add_argument('--romfs-format', choices=FORMATS, default='image', help='image (default): RomFS.img, one file; folder: RomFS/, APFS clones of the dump\'s files, which take almost no disk space (a Mac app only)')
    a = ap.parse_args()
    dump = a.dump.resolve()
    try:
        app, vers, dlc = scan(dump)
        ver = a.version if a.version is not None else max(vers, default=0)
        if ver and ver not in vers: raise Error(f'no updates/{ver} (have: {", ".join(map(str, sorted(vers))) or "none"})')
        base = base_of(dump)
        if not ver and not base: raise Error(f'{dump}: no base game (base/), only updates {", ".join(map(str, sorted(vers)))}')
        src = vers[ver] if ver else base
        work = (a.work_dir or dump / f'{app:016X}_build').resolve()
        out = work / 'extract' / f'v{ver}'
        print(f'{app:016X} version {ver}: {src}', flush=True)
        say = lambda s: print(s, flush=True)
        pack_version(src, app, out, say, base, a.romfs_format)
        pack_dlc(app, dlc, work / 'aoc', say)
    except Error as e: sys.exit(f'[RED] {e}')
    print(f'done: {out}' + (f', {len(dlc)} DLC in {work / "aoc"}' if dlc else ''))


if __name__ == '__main__':
    main()
