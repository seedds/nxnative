# Test adapter for running this project's real Mac build tools on Windows: loaded into every Python the build starts (PYTHONPATH). It changes no repository file.
# It redirects Mac tool calls, makes Python write the same bytes as on a Mac (no CRLF), and has CDLL(None) answer as a system without clonefile.
import builtins, ctypes, io, os, shutil, subprocess, sys
if os.environ.get('DM_WINSHIM') == '1':
    METAL, METALLIB, SPIRV = os.environ['DM_WIN_METAL'], os.environ['DM_WIN_METALLIB'], os.environ['DM_WIN_SPIRV']
    def say(text, code=0): return [sys.executable, '-c', f'import sys; print({text!r}); sys.exit({code})']
    def name_of(a): return os.path.basename(a.replace('\\', '/')).lower().removesuffix('.exe')
    def map_cmd(a):
        n = name_of(a[0])
        if any(isinstance(x, str) and name_of(x) == 'pack_app.py' for x in a[1:2]):   # the app is packed through the kit
            return [a[0], os.path.join(os.path.dirname(os.path.abspath(__file__)), 'win_pack.py'), *a[2:]]
        if n == 'xcrun':
            r = list(a[1:]); sdk = 'macosx'
            while r[:1] in (['-sdk'], ['--sdk']): sdk = r[1]; r = r[2:]
            if r[:1] == ['metal']:
                r = r[1:]
                if '-c' in r and sdk == 'macosx' and not any(x.startswith('-mmacosx-version-min') for x in r): r = ['-mmacosx-version-min=27.2', *r]   # a Mac's SDK gives the target; Windows has no SDK
                return [METAL, *r]
            if r[:1] == ['metallib']: return [METALLIB, *r[1:]]
            if r[:1] in (['metal-tt'], ['metal-pack']): return [os.path.join(os.path.dirname(METAL), r[0] + ('.exe' if os.name == 'nt' else '')), *r[1:]]   # the pipeline archive
            if r[:1] == ['--show-sdk-version']: return say('27.2')
            if r[:1] == ['--show-sdk-build-version']: return say('windows-test')
            return say(f'xcrun stand-in: {r[:1]} is not available on Windows', 1)
        if n == 'sw_vers': return say(os.environ.get('DM_WIN_MACOS', '27.0'))   # the macOS the app is for: this PC is not running it
        if n == 'xcodebuild': return say('Xcode 27.2 (Windows test stand-in)')
        if n == 'zstd': return [sys.executable, '-c', 'import sys\ntry: from compression import zstd\nexcept ImportError: from backports import zstd\nsys.stdout.buffer.write(zstd.decompress(sys.stdin.buffer.read()))']
        if n == 'sysctl': return say('1' if 'hw.optional.arm64' in a else '0')   # the Mac app is for Apple silicon, whatever machine packs it
        if n == 'make': return say('WINDOWS-TEST-STOP: the build reached the Mac host step, the planned end of this test', 77)
        if n == 'dmsh_xlate': return ['dotnet', os.path.join(os.path.dirname(a[0]), 'dmsh_xlate.dll'), *a[1:]]
        if n == 'spirv-cross': return [SPIRV, *a[1:]]
        if n == 'nx_extract' and os.path.exists(a[0] + '.exe'): return [a[0] + '.exe', *a[1:]]
        if os.name == 'nt' and n == 'dotnet' and a[1:2] == ['build'] and '-o' in a:   # the script checks for an executable named nx_extract; on Windows it has .exe
            open(os.path.join(a[a.index('-o') + 1], 'nx_extract'), 'w').close()
        return a
    # What Windows lacks or does differently: positioned reads, binary file descriptors, and '/' in relative game paths
    import threading
    _rel = os.path.relpath
    os.path.relpath = lambda *a, **k: _rel(*a, **k).replace('\\', '/')
    if os.name == 'nt':
        _osopen = os.open
        def os_open(path, flags, *a, **k): return _osopen(path, flags | os.O_BINARY, *a, **k)
        os.open = os_open
    if os.name == 'nt' or os.environ.get('DM_WINSHIM_STRICT') == '1':
        _plock = threading.Lock()
        def pread(fd, n, off):
            with _plock:
                os.lseek(fd, off, os.SEEK_SET); return os.read(fd, n)
        os.pread = pread
    _init = subprocess.Popen.__init__
    def init(self, args, *k, **kw):
        if isinstance(args, (list, tuple)) and args: args = map_cmd([os.fspath(x) if isinstance(x, os.PathLike) else x for x in args])
        if os.environ.get('DM_WINSHIM_STRICT') == '1' and isinstance(args, (list, tuple)) and args:   # the Mac dry run: only what Windows has too
            n = name_of(os.fspath(args[0]))
            if n not in ('dotnet', 'metal', 'metallib', 'spirv-cross', 'nx_extract', 'python3', 'python', 'python3.14') and os.path.abspath(args[0]) != os.path.abspath(sys.executable):
                with _open(os.environ['DM_WINSHIM_LOG'], 'a') as f: f.write(str(args[:3]) + '\n')
                raise FileNotFoundError(2, 'not on Windows: ' + n, args[0])
        if isinstance(args, (list, tuple)) and args and 'WINDOWS-TEST-STOP' in str(args): kw.pop('cwd', None)   # the host's folder is not in the package
        _init(self, args, *k, **kw)
    subprocess.Popen.__init__ = init
    _which = shutil.which
    def which(cmd, *k, **kw):
        if name_of(os.fspath(cmd)) in ('xcodebuild', 'xcodegen', 'make', 'zstd', 'glslangvalidator', 'spirv-cross'): return os.fspath(cmd)
        return _which(cmd, *k, **kw)
    shutil.which = which
    _open = builtins.open
    def open_lf(file, mode='r', buffering=-1, encoding=None, errors=None, newline=None, closefd=True, opener=None):
        if newline is None and 'b' not in mode and any(c in mode for c in 'wax+'): newline = '\n'
        return _open(file, mode, buffering, encoding, errors, newline, closefd, opener)
    builtins.open = open_lf; io.open = open_lf
    _CDLL = ctypes.CDLL
    class NoLibc:
        def __getattr__(self, name): return lambda *a: -1
    class CDLL(_CDLL):
        def __new__(cls, name=None, *a, **k): return NoLibc() if name is None else super().__new__(cls)
    ctypes.CDLL = CDLL
