#!/usr/bin/env python3
# One uncompressed tar in place of a folder of many small files that only a build reads: the Metal and shader compile cache (shader/cache/cas, Metroid Dread's 84,541 files) and
# Xcode's derived data (ipa/build/ios/pkg/dd). A build unpacks the folder, uses it with ordinary paths and packs it at the end, so the tools that read it do not change. The tar sits
# beside the folder as <folder>.tar. A build that stops leaves the folder as it is, and the next one takes it. Times are kept: Xcode's incremental build and prune_shader_cache go by them.
# Every state a stop can leave is handled by unpack:
#   folder alone                a stopped build, or a build folder from before this: used as it is
#   folder and tar              stopped after the unpack's rename or the pack's rename: the folder is complete and at least as new, the tar goes
#   tar and <folder>.gone       stopped while the pack removed the folder: the tar is complete, the rest goes
#   <folder>.unpack, <tar>.part unfinished, they go
# unpack takes a lock on the parent folder that pack releases: regress.py's chain builds share a game's compile cache with the NSP script, and neither may pack it under the other.
import fcntl, os, shutil, subprocess
from pathlib import Path

_locks = {}   # parent folder → descriptor: a second unpack in this process (Spyder runs each F5 in one process, after a build that raised) takes what it already holds


def tar_of(folder): return Path(f'{folder}.tar')
def exists(folder): return Path(folder).is_dir() or tar_of(folder).is_file()


def _remove(p):
    def gone(f, path, e):   # an AppleDouble ._x that an exFAT or FAT drive dropped with x is the goal reached
        if not isinstance(e, FileNotFoundError): raise e
    if p.is_dir() and not p.is_symlink(): shutil.rmtree(p, onexc=gone)
    else: p.unlink(missing_ok=True)


def _lock(folder, say=None):   # without say, it does not wait: False while another process holds the lock
    parent = str(Path(folder).parent)
    if parent in _locks or not os.path.isdir(parent): return True
    fd = os.open(parent, os.O_RDONLY)
    try:
        try: fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            if not say:
                os.close(fd); return False
            say(f'waiting for another build to finish with {folder}')
            fcntl.flock(fd, fcntl.LOCK_EX)
    except OSError:   # a drive without flock: no lock, as before this file
        os.close(fd); return True
    _locks[parent] = fd
    return True


def _unlock(folder):
    fd = _locks.pop(str(Path(folder).parent), None)
    if fd is not None: os.close(fd)


def unpack(folder, say=print):
    """Make folder the ordinary folder its build reads. Returns False when there is nothing yet: the build then makes it. A damaged tar only costs the work it held: both uses are caches"""
    folder = Path(folder); tar = tar_of(folder); work = Path(f'{folder}.unpack'); gone = Path(f'{folder}.gone')
    _lock(folder, say)
    _remove(work); _remove(Path(f'{tar}.part'))
    if folder.is_dir():
        tar.unlink(missing_ok=True); _remove(gone); return True
    _remove(gone)
    if not tar.is_file(): return False
    work.mkdir()
    r = subprocess.run(['tar', '-xf', tar, '-C', work], capture_output=True, text=True, errors='replace')
    if r.returncode:
        say(f'[RED] could not unpack {tar}, the build goes on without it: {r.stderr.strip()[:300]}')
        _remove(work); tar.unlink(); return False
    os.rename(work, folder)
    tar.unlink()
    return True


def pack(folder, say=print):
    """Replace folder with its tar and release unpack's lock. A folder another build is using is left to that build, which packs it at its end.
    A pack that fails (a full drive) is one [RED] line, not an error: the build is done, and the folder stays for the next one"""
    folder = Path(folder); tar = tar_of(folder); part = Path(f'{tar}.part'); gone = Path(f'{folder}.gone')
    if not _lock(folder): return
    try:
        if not folder.is_dir(): return
        try:
            subprocess.run(['tar', '--no-xattrs', '--format=pax', '-cf', part, '-C', folder, '.'], check=True, capture_output=True, env=dict(os.environ, COPYFILE_DISABLE='1'))
            os.replace(part, tar)
            os.rename(folder, gone)
            _remove(gone)
        except (OSError, subprocess.CalledProcessError) as e:
            say(f'[RED] could not pack {folder}, it stays a folder: {getattr(e, "stderr", b"")[:300] if isinstance(e, subprocess.CalledProcessError) else e}')
            try: part.unlink(missing_ok=True)
            except OSError: pass
    finally: _unlock(folder)
