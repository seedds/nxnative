"""A C source of the tools as a ctypes library, built at first use (shader_split.py: yaz0.c). Not an extension module, so one build serves every Python.

The library goes to <temporary folder>/dm-native-<uid>/ (on a Mac $TMPDIR, already per user), never into the repository or the home folder. Its name carries the digest of
the source, the compile command and the machine, so a changed source or another architecture (an x86_64 Python under Rosetta) builds its own, and every worktree shares one.
A build is about 0.1 s, once; processes that build it at the same time each write their own file and rename it into place. On a Mac the compiler is Xcode's
(xcrun clang, which packing already needs), elsewhere cc. load gives None when there is no compiler, on another platform, or when the build or the load fails
(the last two print one line, the compiler's first): the caller then runs its Python code, with the same results."""
import ctypes, hashlib, os, platform, stat, subprocess, sys, tempfile
from pathlib import Path


def command():   # the compile command without its output and source, or None where nothing builds
    if sys.platform == 'darwin': return ['xcrun', 'clang', '-arch', platform.machine(), '-O2', '-shared']
    if sys.platform.startswith('linux'): return ['cc', '-O2', '-shared', '-fPIC']
    return None


def cache_dir():   # the per-user folder, or None when it is not this user's alone (a shared /tmp where another user made it, or one others can write into)
    d = Path(tempfile.gettempdir()) / f'dm-native-{os.getuid()}'
    d.mkdir(mode=0o700, exist_ok=True)
    st = d.lstat()
    return d if stat.S_ISDIR(st.st_mode) and st.st_uid == os.getuid() and not st.st_mode & 0o022 else None


def load(src, cache=None):
    """src (a C file) as a ctypes.CDLL, built into cache (default cache_dir()) if it is not there yet; None when it cannot be built or loaded"""
    src, cmd = Path(src), command()
    if cmd is None: return None
    try:
        d = Path(cache) if cache else cache_dir()
        if d is None: return None
        key = hashlib.sha256(repr((cmd, sys.platform, platform.machine())).encode() + b'\0' + src.read_bytes()).hexdigest()[:16]
        lib = d / f'{src.stem}-{key}{".dylib" if sys.platform == "darwin" else ".so"}'
        if not lib.is_file():
            if sys.platform == 'darwin' and subprocess.run(['xcode-select', '-p'], capture_output=True).returncode: return None   # no Xcode or command line tools (xcrun would offer to install them)
            fd, tmp = tempfile.mkstemp(dir=d, prefix=f'.{src.stem}-', suffix=lib.suffix)
            try:
                os.close(fd)
                r = subprocess.run([*cmd, '-o', tmp, str(src)], capture_output=True, text=True, timeout=120)
                if r.returncode:
                    print(f'{src.name}: the C build failed, the Python code runs: {(r.stderr.strip().splitlines() or [r.returncode])[0]}', file=sys.stderr)
                    return None
                os.replace(tmp, lib)
            finally: Path(tmp).unlink(missing_ok=True)
        return ctypes.CDLL(str(lib))
    except (OSError, subprocess.SubprocessError) as e:
        print(f'{src.name}: no C library, the Python code runs: {e}', file=sys.stderr)
        return None
