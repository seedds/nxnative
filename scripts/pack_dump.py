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
# Writes, under the build folder (<dump folder>/<title ID>_build unless --work-dir names another):
#   extract/v<version>/ExeFS/   the newest version's code modules (APFS clones, or copies on another volume)
#   extract/v<version>/RomFS.img   its RomFS as one image (dm/tools/romfs_image.py: the host's DM_ROMFS, the IPA's romfs.img)
#   aoc/<index>.img   each DLC's RomFS (index = DLC title ID − base title ID − 0x1000), the host's DM_AOC_DIR
# Optional control/control.nacp is preserved beside the code modules; no icon is synthesized.
# The title ID is the program ID in main.npdm. A repeat run keeps code modules that are the same and an image whose source tree, packer and image are unchanged (romfs_image.py --reuse).
# Usage: pack_dump.py <dump folder> [--version N] [--work-dir DIR]   (exit 1 on error)
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


def has_code(src):   # base/ or updates/<version>/ with its code
    return (src / 'exefs/main.npdm').is_file()


def is_version(src):   # base/ or updates/<version>/ with its code and its RomFS
    return has_code(src) and (src / 'romfs').is_dir()


def updates(folder):   # {version: updates/<version>/}
    u = folder / 'updates'
    return {int(p.name): p for p in u.iterdir() if p.is_dir() and p.name.isdigit()} if u.is_dir() else {}


def base_of(folder):   # the base game's folder: base/, or the folder itself when it holds the version directly; None when it has neither
    if has_code(folder / 'base'): return folder / 'base'
    return folder if is_version(folder) else None


def is_dump(folder):   # whether a folder has the layout above: a base game's code with a RomFS beside it or in an update, or an update with its code and RomFS and no base
    base, ups = base_of(folder), updates(folder).values()
    if base: return (base / 'romfs').is_dir() or any((p / 'romfs').is_dir() for p in ups)
    return any(is_version(p) for p in ups)


def program_dir(snapshot, index):
    if not index: return snapshot
    return next((p for p in (snapshot / f'program{index}', snapshot / 'program' / str(index)) if has_code(p)), snapshot / f'program{index}')


def default_program(dump):
    snapshots = [p for _, p in sorted(updates(dump).items(), reverse=True)]
    if (base := base_of(dump)): snapshots.append(base)
    for snapshot in snapshots:
        if not is_version(snapshot): continue
        p = program_dir(snapshot, 1)
        return 1 if is_version(p) and program_id(p / 'exefs/main.npdm') >> 56 == 4 else 0
    return 0


def dlc_romfs(content):
    candidates = [content / 'romfs'] if (content / 'romfs').is_dir() else []
    for instance in sorted(p for p in content.iterdir() if p.is_dir() and p.name != 'romfs'):
        snapshots = [p for _, p in sorted(updates(instance).items(), reverse=True)] + [instance / 'base', instance]
        for snapshot in snapshots:
            rom = next((p for p in (snapshot / 'program/0/romfs', snapshot / 'program0/romfs', snapshot / 'romfs') if p.is_dir()), None)
            if rom: candidates.append(rom); break
    if len(candidates) != 1: raise Error(f'{content}: expected one <n>/romfs folder or extracted DLC snapshot, found {len(candidates)}')
    return candidates[0]


def dlc_index(app, tid):
    # The platform byte and program index may differ between a Switch 2 companion and its Switch 1 DLC.
    return (tid & 0x00FFFFFFFFFFFFFF) - (app & 0x00FFFFFFFFFFF000) - 0x1000


def scan(dump, program=0):
    """What a dump folder holds: (title ID, {update version: its folder}, {DLC title ID: its RomFS folder}). The title ID is the selected program's ID (program 0 by default)"""
    if not is_dump(dump): raise Error(f'{dump}: no base/exefs/main.npdm with a RomFS in base/ or updates/<version>/, nor exefs/main.npdm and romfs in the folder or in updates/<version>/')
    vers = updates(dump)
    app = program_id((base_of(dump) or max((p for p in vers.values() if is_version(p)), key=lambda p: int(p.name))) / 'exefs/main.npdm')
    if app & 0xFFF: raise Error(f'{dump}: base program ID {app:016X} is not a base title ID')
    dlc = {}
    for d in sorted(p for p in (dump / 'dlc').iterdir() if p.is_dir()) if (dump / 'dlc').is_dir() else []:
        try: tid = int(d.name, 16)
        except ValueError: raise Error(f'{d}: not a DLC title ID') from None
        if not 0 < dlc_index(app, tid) <= 2000: raise Error(f'{d}: {tid:016X} is not a DLC of {app:016X}')
        dlc[tid] = dlc_romfs(d)
    if program:
        base = program_dir(base_of(dump), program) if base_of(dump) else None
        vers = {v: program_dir(p, program) for v, p in vers.items() if is_version(program_dir(p, program))}
        candidates = [p for _, p in sorted(vers.items(), reverse=True)] + ([base] if base and has_code(base) else [])
        if not candidates: raise Error(f'{dump}: no complete program {program}')
        pid = program_id(candidates[0] / 'exefs/main.npdm')
        if (pid & 0x00FFFFFFFFFFFFFF) != (app & 0x00FFFFFFFFFFFFFF) + program:
            raise Error(f'{candidates[0]}: program ID {pid:016X} is not program {program} of {app:016X}')
        app = pid
    return app, vers, dlc


def link_directory(src, dst):
    if dst.is_symlink() and dst.resolve() == src.resolve(): return
    if dst.is_symlink() or dst.is_file(): dst.unlink()
    elif dst.exists(): shutil.rmtree(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.symlink_to(src.resolve(), target_is_directory=True)


def pack_version(src, app, out, say=print, raw=False):   # one version's folder (base/ or updates/<version>/) → out/ExeFS and out/RomFS.img
    if not (src / 'exefs/main').is_file() or not (src / 'exefs/main.npdm').is_file() or not (src / 'romfs').is_dir(): raise Error(f'{src}: no exefs/main, exefs/main.npdm and romfs')
    if (pid := program_id(src / 'exefs/main.npdm')) != app: raise Error(f'{src}: program ID {pid:016X}, the base is {app:016X}')
    exefs(src / 'exefs', out / 'ExeFS')
    if raw:
        for name in ('RomFS.img', 'RomFS.img.inputs.json'): (out / name).unlink(missing_ok=True)
        link_directory(src / 'romfs', out / 'RomFS')
    else: pack(src / 'romfs', out / 'RomFS.img', say)
    control = src / 'control/control.nacp'
    if control.is_file(): clone(control, out / 'control.nacp')


def pack_dlc(app, dlc, aoc, say=print, raw=False):   # {DLC title ID: its RomFS folder} → aoc/<index>.img; images of DLC no longer in the folder go
    want = {f'{dlc_index(app, tid)}' + ('' if raw else '.img'): roms for tid, roms in dlc.items()}
    if aoc.is_dir():
        for f in aoc.iterdir():
            if f.name.removesuffix('.inputs.json') not in want:
                if f.is_dir() and not f.is_symlink(): shutil.rmtree(f)
                else: f.unlink()
    for name, roms in want.items():
        if raw: link_directory(roms, aoc / name)
        else: pack(roms, aoc / name, say)


def main():
    ap = argparse.ArgumentParser(description='Pack a dumped game (base/, updates/<version>/, dlc/) into the extraction layout of build_ipa_from_nsp.py')
    ap.add_argument('dump', type=Path, help='the dump folder (holds base/exefs with base/romfs or updates/<version>/romfs, or updates/<version>/ with exefs and romfs, or exefs and romfs directly)')
    ap.add_argument('--version', type=int, help='the update version to pack (default: the newest in updates/; 0 = the base game)')
    ap.add_argument('--work-dir', type=Path, help='the build folder (default: <dump>/<title ID>_build)')
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
        pack_version(src, app, out, say)
        pack_dlc(app, dlc, work / 'aoc', say)
    except Error as e: sys.exit(f'[RED] {e}')
    print(f'done: {out}' + (f', {len(dlc)} DLC in {work / "aoc"}' if dlc else ''))


if __name__ == '__main__':
    main()
