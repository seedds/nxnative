#!/usr/bin/env python3
# Remove the build folders Metal leaves in the user cache folder (`getconf DARWIN_USER_CACHE_DIR`, then com.apple.gpuarchiver/gpuarchiver-*, and the built apps'
# org.nxnative.*/com.apple.gpuarchiver/gpuarchiver-*). The Metal 4 serializer makes them for the host's pipeline archive flushes and never removes them. A host from a build shares
# the first folder with every other program and must not remove from it: Metal does not say which folder is its own, and a folder of another live compiler went with the former
# cleanup (docs/reference/shaders.md, "Metal's build folders left by pipeline archive flushes"). A built app removes its own earlier runs' folders at its next start; this covers
# the apps not started again. They grow without bound, up to about 127 MB each.
# A folder goes only when nothing in it changed for --idle-min minutes (default 60) and its last change is older than the start of every running dmhost, so a running host keeps its own;
# an app's folders only while no copy of that app holds its lock (gpuarchiver.lock beside them).
# Usage: python3 scripts/clean_metal_cache.py [--idle-min N] [--dry-run] [--dir <folder>]. Exit code 0, also when there is nothing to remove; 1 when a folder could not be removed
# or the running processes could not be listed. scripts/regress.py runs it before its free-space check.
import argparse, fcntl, os, shutil, subprocess, sys, time
from pathlib import Path

HOSTS = {'dmhost'}   # the programs whose folders are spared while they run (the Mac app's launcher starts the host under this name too)


def cache_dirs():   # the shared folder, then each built app's (bundle ID org.nxnative.<title ID>, pack_app.py)
    r = subprocess.run(['getconf', 'DARWIN_USER_CACHE_DIR'], capture_output=True, text=True)
    if r.returncode or not r.stdout.strip(): return []
    c = Path(r.stdout.strip())
    return [c / 'com.apple.gpuarchiver'] + sorted(c.glob('org.nxnative.*/com.apple.gpuarchiver'))


def etime_s(s):   # ps's elapsed time, [[dd-]hh:]mm:ss
    d, _, hms = s.rpartition('-')
    p = [int(x) for x in hms.split(':')]
    p = [0] * (3 - len(p)) + p
    return int(d or 0) * 86400 + p[0] * 3600 + p[1] * 60 + p[2]


def host_starts(now):
    """The start times of the running hosts; an error when ps cannot list the processes, so nothing is removed without knowing"""
    r = subprocess.run(['ps', '-axo', 'etime=,comm='], capture_output=True, text=True)
    if r.returncode: raise OSError(f'ps failed: {r.stderr.strip()}')
    out = []
    for line in r.stdout.splitlines():
        t, _, comm = line.strip().partition(' ')
        if os.path.basename(comm.strip()) in HOSTS: out.append(now - etime_s(t))
    return out


def scan(d):   # → (last change of the folder or anything in it, bytes on disk)
    st = d.lstat(); t, b = st.st_mtime, st.st_blocks * 512
    for root, dirs, files in os.walk(d):
        for n in dirs + files:
            try: st = os.lstat(os.path.join(root, n))
            except FileNotFoundError: continue
            t = max(t, st.st_mtime); b += st.st_blocks * 512
    return t, b


def listing(d):
    """d's gpuarchiver-* folders; None when a copy of the app that owns d is running (it holds the lock beside d shared for its run, nvn_mtl.m ga_claim). Listed under the
    exclusive lock and removed after it, so an app starting meanwhile waits only for the listing, and its own folders, made after, are not among them"""
    lock = d.parent / 'gpuarchiver.lock'
    if not lock.exists(): return sorted(d.glob('gpuarchiver-*'))   # the shared folder, or an app from before the lock (the idle rule spares its running hosts)
    with open(lock, 'a') as f:
        try: fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError: return None
        return sorted(d.glob('gpuarchiver-*'))


def clean(folder=None, idle_min=60, dry_run=False, now=None, starts=None, say=print):
    """→ (folders removed, their bytes, folders kept, folders that could not be removed). starts: the running hosts' start times (default: from ps)"""
    dirs = [d for d in ([Path(folder)] if folder else cache_dirs()) if d.is_dir()]
    if not dirs: return 0, 0, 0, 0
    now = time.time() if now is None else now
    starts = host_starts(now) if starts is None else starts
    limit = min([now - idle_min * 60] + [s - 60 for s in starts])   # a minute's margin: ps gives whole seconds, and a host makes its first folder after it started
    n = size = kept = bad = 0
    names = []
    for d in dirs:
        ls = listing(d)
        if ls is None: say(f'clean_metal_cache: {d} kept: a copy of its app is running'); continue
        names += ls
    for f in names:
        if f.is_symlink() or not f.is_dir(): continue
        try: t, b = scan(f)
        except FileNotFoundError: continue   # removed meanwhile (another cleanup: regress.py's workers check the disk at the same time)
        if t >= limit: kept += 1; continue
        if not dry_run:
            try: shutil.rmtree(f)
            except FileNotFoundError: continue
            except OSError as e: say(f'[RED] clean_metal_cache: {f} not removed: {e}'); bad += 1; continue
        n += 1; size += b
    return n, size, kept, bad


def main():
    ap = argparse.ArgumentParser(description="Remove the idle build folders Metal's pipeline archive compiler leaves in the user cache folder")
    ap.add_argument('--idle-min', type=float, default=60, help='remove a folder only when nothing in it changed for this many minutes (default 60)')
    ap.add_argument('--dry-run', action='store_true', help='list what would go, remove nothing')
    ap.add_argument('--dir', help="the folder that holds the gpuarchiver-* folders (default: com.apple.gpuarchiver in the user cache folder and in each built app's folder there)")
    a = ap.parse_args()
    try: n, size, kept, bad = clean(a.dir, a.idle_min, a.dry_run)
    except OSError as e: print(f'[RED] clean_metal_cache: {e}: nothing removed', file=sys.stderr); return 1
    print(f'{"would remove" if a.dry_run else "removed"} {n} idle Metal build folders, {size / 1e9:.1f} GB; kept {kept} (changed in the last {a.idle_min:g} minutes '
          f'or since a running host started)')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
