#!/usr/bin/env python3
# A game's NVN shader libraries as one file: b-<chain>.img beside the folder b-<chain>/ that dmsh_build.py writes. One file per shader variant is what made a build folder slow to copy or
# delete (Skyward Sword HD: 12,064 files for 2,470 shaders; 28,669 shaders in Tears of the Kingdom), and each regress.py chain build held another set. Nothing is unpacked to use it: the host
# (dm/gfx/nvn/msl_image.h) and the tools (romfs.py) read a shader in place through the index.
# The format is the RomFS image's (romfs_image.py), one flat folder, with each file's data aligned to ALIGN bytes: Metal is handed a pointer into the mapped file.
# A folder and an image may lie side by side, and each file comes from the folder first:
#   folder with a stamp (inputs.tsv)      a finished full build: the image, if any, is older and pack replaces it
#   folder without a stamp, and an image  a partial dmsh_build.py run (given keys) over a packed library, or a stopped build: the folder holds only what it rebuilt, and pack keeps the image's other files
#   folder alone                          a build folder from before this, a partial build, or a build in progress: used as it is
# pack writes the image, then removes the folder, so every file is in one or the other at each moment, and a host running from the folder goes on from the image. A pack that fails
# (a full drive) is one [RED] line, not an error: the build is done, and the folder works.
# An iPhone library may leave out the pulling vertex libraries when nothing will read them (pack's leave_out), which the marker PULLING_LEFT_OUT inside it says: the build that needs them compiles them,
# and pack_ipa.py refuses to pack such an image for an app that needs them, which would fail only on the device.
# Not part of the translation chain: it decides no output (dmsh_chain.py), and dmsh_build.py still writes a folder.
# Usage: shader_lib.py pack <lib>                      the folder into the image
#        shader_lib.py expand <lib>                    the image back into a folder, only for a host from before this change
#        shader_lib.py path <lib>                      what to read: the folder or the image
#        shader_lib.py has <lib>                       exit status 0 when it holds a shader's library (<key>_b.metallib)
#        shader_lib.py cat <lib> <name>                one file's bytes on standard output
#        shader_lib.py subset <lib> <out.img> [--skip-suffix S]...  a new image without .metal sources and build reports, as an app carries it
# <lib> is the path b-<chain>, with or without .img.
import argparse, builtins, os, shutil, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import romfs

ALIGN = 16   # of each file's data: the file is mapped by pages, so its first byte's alignment is its offset's in the image
STAMP = 'inputs.tsv'   # dmsh_chain.py STAMP
APP_SUFFIXES = ('.metallib', '.json', '.tsv')   # what the host reads (not the Mac build's .metal sources)
APP_LEAVE_OUT = ('_b_build.json',)   # dmsh_build.py's report, for the tools
PULLING, PULLING_LEFT_OUT = '_bp.metallib', '_bp_left_out'   # dmsh_pull.py's libraries (build_ipa.sh decides who wants them), and the marker of an image without them


def folder_of(lib):
    s = os.fspath(lib)
    return s[:-4] if s.endswith('.img') else s


def image_of(lib): return folder_of(lib) + '.img'
def name_of(lib): return os.path.basename(os.path.normpath(folder_of(lib)))   # b-<chain>, as the headers of c1_full.tsv and cb_extent.tsv name it


def view(lib):
    """lib as a romfs.Path to read from (the state table at the top of this file). A path that does not exist gives the folder's, whose glob is empty and whose reads fail as they did"""
    folder, image = folder_of(lib), image_of(lib)
    if os.path.isfile(image) and not os.path.isfile(os.path.join(folder, STAMP)): return romfs.Path(image)
    return romfs.Path(folder)


def exists(lib): return os.path.isdir(folder_of(lib)) or os.path.isfile(image_of(lib))


def read_stamp(lib):   # dmsh_chain.py read_stamp, on a folder or an image
    try: return dict(l.split('\t', 1) for l in (view(lib) / STAMP).read_text().splitlines() if '\t' in l and not l.startswith('#'))
    except OSError: return {}


def table_head(lib):   # dmsh_chain.py table_head, on a folder or an image: the name is the folder's either way
    return f'# chain {name_of(lib)} inputs {read_stamp(lib).get("inputs", "none")}'


def _names(folder):
    """{name: (size, mtime)} of the files in a library folder. A file name starting with a dot is detritus (an AppleDouble ._x on an exFAT or FAT drive, .DS_Store), and so is what a stopped dmsh_build.py left (<name>.tmp-<pid>-<thread>)"""
    out = {}
    with os.scandir(folder) as it:
        for e in it:
            if e.name.startswith('.') or '.tmp' in e.name: continue
            if e.is_dir(follow_symlinks=False): raise ValueError(f'{e.path} is a folder: a shader library holds only files')
            st = e.stat(); out[e.name] = (st.st_size, st.st_mtime_ns)
    return out


def write(files, dst):
    """files: {name: path to read (a romfs path may name a file inside an image) or the bytes} → the image dst, by way of dst.part. A stopped write leaves only the .part, and an image is whole or absent"""
    part = dst + '.part'
    names = sorted((n.encode() for n in files))
    ents, off = [], romfs.HEAD.size
    with builtins.open(part, 'wb') as out:
        out.write(b'\0' * off)
        for name in names:
            off += -off % ALIGN
            out.seek(off)
            n, source = 0, files[name.decode()]
            if isinstance(source, bytes): out.write(source); n = len(source)
            else:
                with romfs.open(source, 'rb') as f:
                    while b := f.read(8 << 20): out.write(b); n += len(b)
            ents.append((name, off, n)); off += n
        out.seek(off)
        pool, idx = bytearray(), bytearray(romfs.ENT.pack(1, len(names), 0, 0, 0, 0))   # entry 0, the root: its children are every other entry
        for name, a, b in ents:
            idx += romfs.ENT.pack(a, b, len(pool), len(name), 1, 0); pool += name
        out.write(idx); out.write(pool)
        out.seek(0); out.write(romfs.HEAD.pack(romfs.MAGIC, off, len(idx) + len(pool), len(names) + 1))
    romfs.Image(part)   # a read of it, as the host's check: nothing is published that a reader refuses
    os.replace(part, dst)


def pack(lib, say=print, leave_out=False):
    """The folder into the image, and the folder removed. Returns whether an image now stands for the library. Without a folder there is nothing to do, unless leave_out finds pulling libraries in the
    image to take out: leave_out is the iPhone build's when nothing will read them"""
    folder, image = folder_of(lib), image_of(lib)
    try:
        before = _names(folder) if os.path.isdir(folder) else None
        if before is not None and not before: os.rmdir(folder); before = None
        if before is None and not os.path.isfile(image): return False
        files = {n: os.path.join(folder, n) for n in before or ()}
        if os.path.isfile(image) and (before is None or STAMP not in before):   # a partial build over a packed library, or the image alone
            with romfs.scandir(image) as it:
                for e in it: files.setdefault(e.name, e.path)
        pulling = [n for n in files if n.endswith(PULLING)]
        if before is None and not (leave_out and pulling): return True
        if leave_out:
            for n in pulling: del files[n]
            files[PULLING_LEFT_OUT] = b'the pulling vertex libraries are left out: nothing in this build reads them (dm/tools/shader_lib.py)\n'
        write(files, image)
        if before is None: return True
        if _names(folder) != before:   # a build wrote to it meanwhile: the folder is the newer one, and the next pack takes it
            say(f'{folder} changed while it was packed: it stays a folder'); return True
        shutil.rmtree(folder)
    except (OSError, ValueError) as e:
        say(f'[RED] could not pack {folder}, it stays a folder: {e}')
        try: os.unlink(image + '.part')
        except OSError: pass
        return False
    return True


def remove_image(lib):   # before a full build: its folder replaces the library, and a stopped build must not take old shaders for the ones it had not reached
    for p in (image_of(lib), image_of(lib) + '.part'):
        try: os.unlink(p)
        except FileNotFoundError: pass


def expand(lib):
    """The image back into a folder and the image removed, for a host from before this file (a kept host copy of the same chain). No build or run of the current sources needs it"""
    folder, image = folder_of(lib), image_of(lib)
    if not os.path.isfile(image): return
    work = folder + '.expand'
    shutil.rmtree(work, ignore_errors=True); os.makedirs(work)
    for p in romfs.Path(image).iterdir():
        with builtins.open(os.path.join(work, p.name), 'wb') as out: out.write(p.read_bytes())
    if os.path.isdir(folder):   # what a partial build made wins, as in pack
        for n in _names(folder): os.replace(os.path.join(folder, n), os.path.join(work, n))
        shutil.rmtree(folder)
    os.rename(work, folder); os.unlink(image)


def app_files(lib, skip_suffix=()):
    """{name: path} of what an app carries of the library: what the host reads, and of that what the build wants (build_ipa.sh leaves out the pulling vertex libraries when nothing uses them)"""
    return {p.name: str(p) for p in view(lib).iterdir() if p.name.endswith(APP_SUFFIXES) and p.name not in APP_LEAVE_OUT and not p.name.startswith('.') and '.tmp' not in p.name and not p.name.endswith(tuple(skip_suffix) or ('\0',))}


def subset(lib, dst, skip_suffix=()):   # → the number of files in the image dst
    files = app_files(lib, skip_suffix)
    if not any(n.endswith('.metallib') for n in files): raise ValueError(f'no shader libraries at {lib}')
    write(files, os.fspath(dst))
    return len(files)


def has_library(lib): return next(view(lib).glob('*_b.metallib'), None) is not None
def left_out_pulling(lib): return (view(lib) / PULLING_LEFT_OUT).is_file()   # whether a build that needs the pulling libraries must make them again


def main():
    ap = argparse.ArgumentParser(description='A game\'s shader libraries as one file, read in place')
    ap.add_argument('cmd', choices=('pack', 'expand', 'path', 'has', 'cat', 'subset'))
    ap.add_argument('lib'); ap.add_argument('arg', nargs='?')
    ap.add_argument('--skip-suffix', action='append', default=[])
    a = ap.parse_args()
    if a.cmd == 'pack': return 0 if pack(a.lib) else 1
    if a.cmd == 'expand': expand(a.lib); return 0
    if a.cmd == 'path': print(view(a.lib)); return 0
    if a.cmd == 'has': return 0 if has_library(a.lib) else 1
    if a.cmd == 'cat':
        try: sys.stdout.buffer.write((view(a.lib) / a.arg).read_bytes())
        except OSError as e: sys.exit(f'[RED] {e}')
        return 0
    try: print(f'{a.arg}: {subset(a.lib, a.arg, a.skip_suffix)} files, {os.path.getsize(a.arg)} bytes')
    except (OSError, ValueError) as e: sys.exit(f'[RED] {e}')
    return 0


if __name__ == '__main__': sys.exit(main())
