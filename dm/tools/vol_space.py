"""The space on a path's volume that a write the user started can use, for the checks that stop a build before it writes (the NSP script, pack_app.py, pso_offline.py).

statvfs (shutil.disk_usage) counts only unallocated blocks. On APFS, space held by purgeable data (local Time Machine snapshots, caches the system deletes on demand)
is not in it, and macOS frees that space while the build writes: Finder showed 68 GB available where statvfs showed 18.8 GB, and a build wrote 92 GB there. macOS
reports that figure as kCFURLVolumeAvailableCapacityForImportantUsageKey, the capacity for writes a user asked for. free_bytes takes it, never less than statvfs's
figure, so no check becomes stricter. Housekeeping nobody asked for (tree_tar.pack_quiet) and a failure's diagnosis (what was really left) keep statvfs."""
import ctypes, functools, os, shutil, sys
from pathlib import Path

CF_PATH = '/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation'
K_CF_NUMBER_SINT64 = 4   # kCFNumberSInt64Type


@functools.cache
def _cf():   # CoreFoundation and the key, loaded once; None off macOS or when it cannot be loaded
    if sys.platform != 'darwin': return None
    try:
        cf = ctypes.CDLL(CF_PATH)
        cf.CFURLCreateFromFileSystemRepresentation.restype = ctypes.c_void_p
        cf.CFURLCreateFromFileSystemRepresentation.argtypes = (ctypes.c_void_p, ctypes.c_char_p, ctypes.c_long, ctypes.c_bool)
        cf.CFURLCopyResourcePropertyForKey.restype = ctypes.c_bool
        cf.CFURLCopyResourcePropertyForKey.argtypes = (ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)
        cf.CFNumberGetValue.restype = ctypes.c_bool
        cf.CFNumberGetValue.argtypes = (ctypes.c_void_p, ctypes.c_long, ctypes.c_void_p)
        cf.CFRelease.restype = None
        cf.CFRelease.argtypes = (ctypes.c_void_p,)
        return cf, ctypes.c_void_p.in_dll(cf, 'kCFURLVolumeAvailableCapacityForImportantUsageKey')
    except (AttributeError, OSError, ValueError): return None


def important_bytes(path):
    """macOS's capacity for important usage on the volume of path (an existing path), in bytes; None where macOS does not give one (another system, a volume without it)"""
    lib = _cf()
    if not lib: return None
    cf, key = lib
    b = os.fsencode(Path(path).resolve())
    url = cf.CFURLCreateFromFileSystemRepresentation(None, b, len(b), os.path.isdir(path))
    if not url: return None
    try:
        v, n = ctypes.c_void_p(), ctypes.c_int64()
        if not cf.CFURLCopyResourcePropertyForKey(url, key, ctypes.byref(v), None) or not v: return None
        try: return n.value if cf.CFNumberGetValue(v, K_CF_NUMBER_SINT64, ctypes.byref(n)) and n.value >= 0 else None
        finally: cf.CFRelease(v)
    finally: cf.CFRelease(url)


def free_bytes(path):
    """The bytes a build may write on the volume of path, which need not exist yet (its nearest existing parent is asked)"""
    p = Path(path)
    while not p.exists() and p != p.parent: p = p.parent
    raw = shutil.disk_usage(p).free
    important = important_bytes(p)
    return raw if important is None else max(raw, important)
