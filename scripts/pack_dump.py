#!/usr/bin/env python3
# Game files already extracted (a console dump: folders, not NSPs) → the layout build_ipa_from_nsp.py extracts NSPs into, so the rest of its steps and the Mac host run from it.
# The dump folder holds:
#   base/exefs/ (main, main.npdm, sdk, rtld, subsdk*), base/romfs/
#   updates/<version>/exefs/, updates/<version>/romfs/   (optional; <version> is the title version as a decimal number, 458752 = 0x70000)
#   dlc/<DLC title ID>/<n>/romfs/   (optional; the DLC title ID in hex)
# An update's RomFS in such a dump is the patched RomFS (base + the update's BKTR patch, the tree the game sees), not a patch: it is packed on its own, never laid over the base
# (Donkey Kong Bananza 458752 drops 42 base files, among them the Product.100 tables its Product.200 ones replace).
# Writes, under the build folder (<dump folder>/<title ID>_build unless --work-dir names another):
#   extract/v<version>/ExeFS/   the newest version's code modules (APFS clones, or copies on another volume)
#   extract/v<version>/RomFS.img   its RomFS as one image (dm/tools/romfs_image.py: the host's DM_ROMFS, the IPA's romfs.img)
#   aoc/<index>.img   each DLC's RomFS (index = DLC title ID − base title ID − 0x1000), the host's DM_AOC_DIR
# The title ID is the program ID in main.npdm. A repeat run keeps an image whose source tree, packer and image are unchanged (romfs_image.py --reuse).
# Usage: pack_dump.py <dump folder> [--version N] [--work-dir DIR]   (exit 1 on error)
import argparse, shutil, struct, subprocess, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'dm/tools'))
import romfs_image


def die(msg):
    sys.exit(f'[RED] {msg}')


def program_id(npdm):   # the ACI0 program ID of main.npdm
    d = npdm.read_bytes()
    if d[:4] != b'META': die(f'{npdm}: not an NPDM')
    off, size = struct.unpack_from('<II', d, 0x70)
    if d[off:off + 4] != b'ACI0' or size < 0x18: die(f'{npdm}: no ACI0 section')
    return struct.unpack_from('<Q', d, off + 0x10)[0]


def clone(src, dst):   # an APFS clone on the same volume, else a copy
    if subprocess.run(['cp', '-c', src, dst], capture_output=True).returncode != 0: shutil.copy2(src, dst)


def exefs(src, dst):
    tmp = dst.with_name(dst.name + '.partial')
    shutil.rmtree(tmp, ignore_errors=True); tmp.mkdir(parents=True)
    for f in sorted(src.iterdir()):
        if f.is_file() and not f.name.startswith('.'): clone(f, tmp / f.name)
    shutil.rmtree(dst, ignore_errors=True); tmp.rename(dst)


def pack(src, img):
    img.parent.mkdir(parents=True, exist_ok=True)
    print(f'packing {src} → {img}', flush=True)
    n = romfs_image.pack(src, img, reuse=True)
    print(f'  {n:,} entries, {img.stat().st_size / 2**30:.2f} GB', flush=True)


def main():
    ap = argparse.ArgumentParser(description='Pack a dumped game (base/, updates/<version>/, dlc/) into the extraction layout of build_ipa_from_nsp.py')
    ap.add_argument('dump', type=Path, help='the dump folder (holds base/exefs and base/romfs)')
    ap.add_argument('--version', type=int, help='the update version to pack (default: the newest in updates/; 0 = the base game)')
    ap.add_argument('--work-dir', type=Path, help='the build folder (default: <dump>/<title ID>_build)')
    a = ap.parse_args()
    dump = a.dump.resolve()
    base = dump / 'base'
    if not (base / 'exefs/main.npdm').is_file() or not (base / 'romfs').is_dir(): die(f'{dump}: no base/exefs/main.npdm and base/romfs')
    app = program_id(base / 'exefs/main.npdm')
    if app & 0xFFF: die(f'{dump}: base program ID {app:016X} is not a base title ID')
    vers = {int(p.name): p for p in (dump / 'updates').iterdir() if p.is_dir() and p.name.isdigit()} if (dump / 'updates').is_dir() else {}
    ver = a.version if a.version is not None else max(vers, default=0)
    if ver and ver not in vers: die(f'no updates/{ver} (have: {", ".join(map(str, sorted(vers))) or "none"})')
    src = vers[ver] if ver else base
    if not (src / 'exefs/main').is_file() or not (src / 'romfs').is_dir(): die(f'{src}: no exefs/main and romfs')
    if (pid := program_id(src / 'exefs/main.npdm')) != app: die(f'{src}: program ID {pid:016X}, the base is {app:016X}')
    work = (a.work_dir or dump / f'{app:016X}_build').resolve()
    out = work / 'extract' / f'v{ver}'
    print(f'{app:016X} version {ver}: {src}', flush=True)
    exefs(src / 'exefs', out / 'ExeFS')
    pack(src / 'romfs', out / 'RomFS.img')
    dlc = sorted(p for p in (dump / 'dlc').iterdir() if p.is_dir()) if (dump / 'dlc').is_dir() else []
    for d in dlc:
        try: tid = int(d.name, 16)
        except ValueError: die(f'{d}: not a DLC title ID')
        idx = tid - app - 0x1000
        if not 0 < idx <= 2000: die(f'{d}: {tid:016X} is not a DLC of {app:016X}')
        roms = sorted(d.glob('*/romfs')) or ([d / 'romfs'] if (d / 'romfs').is_dir() else [])
        if len(roms) != 1: die(f'{d}: expected one <n>/romfs folder, found {len(roms)}')
        pack(roms[0], work / 'aoc' / f'{idx}.img')
    print(f'done: {out}' + (f', {len(dlc)} DLC in {work / "aoc"}' if dlc else ''))


if __name__ == '__main__':
    main()
