#!/usr/bin/env python3
# An update in a NUP container (pack_nup.py) → an update pack in a dump folder (pack_dump.py): updates/<version>/exefs/ (the update's code, whole), updates/<version>/romfs/ (only the
# RomFS files the update adds or changes against the container's base game) and updates/<version>/romfs_removed.txt (the base files the update drops). The build lays that romfs/ over
# base/romfs (pack_dump.py, romfs_image.py layered_entries), which gives the update's RomFS as the container holds it.
# What it is for: a NUP update holds its whole RomFS, which the build would unpack beside the base game's. The files are compared by the SHA-256 the container stores for each, so only the
# ones that differ are read: Ocarina of Time's v131072 is 2,285 files (350 MB) of a 26.3 GB RomFS. With the pack written, the dump folder holds base and update and the .nup can go.
# The dump folder's base/ must be this container's base game: written with --base when the folder has none, else checked against the container (every file's size, and the SHA-256 of
# every base file the update changes or drops, the ones the pack does not carry) before the pack is written; --no-check-base skips that check.
# Usage: nup_update_pack.py <game.nup> <dump folder> [--version N] [--base] [--no-check-base]   (exit 1 on error)
import argparse, hashlib, os, shutil, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import pack_nup
from pack_nup import NO_DIR, Error


def tree(n, d):   # {guest path: file index} of every file under folder d of the container
    out, todo = {}, [(d, '')]
    while todo:
        k, pre = todo.pop()
        for name, isdir, i in n.children(k):
            p = pre + os.fsdecode(name)
            if '\0' in p or any(c in ('', '.', '..') for c in p.split('/')): raise Error(f'{n.path}: invalid RomFS name {p!r}')
            if isdir: todo.append((i, p + '/'))
            else: out[p] = i
    return out


def write_file(n, i, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists(): raise Error(f'{path}: another RomFS name differs from it only in letter case, which this volume cannot keep apart')
    with open(path, 'wb') as f: n.copy(i, f)


def sha256(path):
    with open(path, 'rb') as f: return hashlib.file_digest(f, 'sha256').digest()


def check_base(n, base, files, touched, say):   # the dump's base/romfs against the container's base: every file's size, and the hash of each file the update changes or drops
    bad, hashed = [], 0
    for p, i in files.items():
        f = base / 'romfs' / p
        try: size = f.stat().st_size
        except OSError: bad.append(f'{p}: missing'); continue
        if size != n.files[i][1]: bad.append(f'{p}: {size:,} bytes, the container\'s {n.files[i][1]:,}'); continue
        if p in touched:
            hashed += 1
            if sha256(f) != n.files[i][2]: bad.append(f'{p}: not the container\'s bytes')
    if bad: raise Error(f'{base}/romfs is not this container\'s base game ({len(bad):,} files differ: ' + '; '.join(bad[:5]) + (' …' if len(bad) > 5 else '') + '); write the base again with --base into an empty folder')
    say(f'base/romfs matches the container: {len(files):,} files by size, {hashed:,} by SHA-256')


def main():
    ap = argparse.ArgumentParser(description='Write a NUP update as an update pack (only its changed RomFS files) in a dump folder')
    ap.add_argument('nup', type=Path, help='the .nup file')
    ap.add_argument('dump', type=Path, help='the dump folder: the pack goes to <dump>/updates/<version>/')
    ap.add_argument('--version', type=int, help='the update version (default: the newest in the container)')
    ap.add_argument('--base', action='store_true', help='also write the base game, base/exefs and base/romfs, when the folder has no base/ (its whole RomFS: as big as the game)')
    ap.add_argument('--no-check-base', action='store_true', help='do not check the folder\'s base/ against the container')
    a = ap.parse_args()
    say = lambda s: print(s, flush=True)
    try:
        with pack_nup.Nup(a.nup.resolve()) as n:
            app, base, vers, _ = pack_nup.scan(n)
            if base is None: raise Error(f'{a.nup.name}: no base game, so there is nothing to compare its update with: pack it with pack_nup.py as the whole game')
            if not vers: raise Error(f'{a.nup.name}: no update')
            ver = a.version if a.version is not None else max(vers)
            if ver not in vers: raise Error(f'no update {ver} in {a.nup.name} (have: {", ".join(map(str, sorted(vers)))})')
            k = vers[ver]
            if k.exefs == NO_DIR or n.child(k.exefs, b'main') is None or n.child(k.exefs, b'main.npdm') is None: raise Error(f'{k.name}: no exefs/main and exefs/main.npdm')
            if base.romfs == NO_DIR: raise Error(f'{base.name}: no romfs/')
            dump = a.dump.resolve(); bdir = dump / 'base'
            old = tree(n, base.romfs)
            new = tree(n, k.romfs) if k.romfs != NO_DIR else dict(old)   # an update without romfs/ runs with the base's
            changed = sorted(p for p, i in new.items() if p in old and n.files[i][2] != n.files[old[p]][2])
            added = sorted(p for p in new if p not in old)
            gone = sorted(p for p in old if p not in new)
            size = sum(n.files[new[p]][1] for p in changed + added)
            say(f'{app:016X} update {ver} against the base: {len(changed):,} changed, {len(added):,} added, {len(gone):,} removed of {len(new):,} files ({size / 2**20:,.0f} MB to write)')
            if (bdir / 'exefs').exists() or (bdir / 'romfs').exists():
                if a.base: raise Error(f'{bdir} exists: --base writes a base game only into a folder without one')
                if not a.no_check_base: check_base(n, bdir, old, set(changed) | set(gone), say)
            elif a.base:
                part = dump / 'base.partial'; shutil.rmtree(part, ignore_errors=True)
                say(f'writing the base game → {bdir} ({n.tree_bytes(base.romfs) / 2**30:.2f} GB)')
                pack_nup.exefs(n, base, part / 'exefs')
                for p, i in old.items(): write_file(n, i, part / 'romfs' / p)
                part.rename(bdir)
            else: raise Error(f'{dump} has no base/: give --base to write the container\'s base game there too')
            out = dump / 'updates' / str(ver); part = out.with_name(out.name + '.partial')
            shutil.rmtree(part, ignore_errors=True); part.mkdir(parents=True)
            pack_nup.exefs(n, k, part / 'exefs')
            (part / 'romfs').mkdir()
            for p in changed + added: write_file(n, new[p], part / 'romfs' / p)
            (part / 'romfs_removed.txt').write_text(f'# RomFS paths update {ver} drops from the base game (nup_update_pack.py; pack_dump.py lays romfs/ over base/romfs without them)\n'
                                                    + ''.join(p + '\n' for p in gone), encoding='utf-8')
            if out.exists(): shutil.rmtree(out)
            part.rename(out)
    except (Error, OSError) as e: sys.exit(f'[RED] {e}')
    say(f'done: {out}')


if __name__ == '__main__':
    main()
