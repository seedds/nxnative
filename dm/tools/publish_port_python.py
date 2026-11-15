#!/usr/bin/env python3
"""The packer's own Python for the port's bundles (docs/development.md, "The port's bundles"), so that a player packs a game with nothing installed: CPython 3.14 (its standard
library has compression.zstd, the zstd module the packer's tools read .zs files with) with the three modules the packer imports beyond it, lz4 (the NSOs' segments), capstone
(the rewriter's disassembly) and fontTools (make_ext_font.py, the button-icon font), from their wheels on PyPI. Every download is pinned below by URL and SHA-256; a
mismatch stops with a [RED] line. Run at bundle-build time in Docker (any image with Python 3; the commands of docker/README.md, "The port's tools"), never on a player's PC
and never at pack time; scripts/package_port.py --python puts its output into a bundle (python/ and licenses/), and the bundle's pack.sh / pack.cmd run the packer with it.

    python3 dm/tools/publish_port_python.py linux build/port_python/linux-x64 --check
    python3 dm/tools/publish_port_python.py windows build/port_python/win-x64 --check

The distributions:
  linux    python-build-standalone's install_only_stripped build for x86_64-unknown-linux-gnu (a relocatable CPython: bin/python3.14 links the interpreter and its extension
           modules in, and needs only glibc's own libraries, from glibc 2.17 on), with the manylinux wheels. What a packer never runs is left out (tkinter with Tcl/Tk, IDLE,
           ensurepip and pip, the headers, the man pages and terminfo, the shared libpython for embedding, the symbolic links, which a bundle holds none of). The
           licences of what is linked into the interpreter (OpenSSL, libffi, SQLite, zlib, xz, zstd, ...) come from python/licenses/ of the same release's full archive
           (pgo+lto, the build install_only is made from), downloaded for them alone
  windows  python.org's embeddable package for amd64 (python.exe, python314.dll, the standard library as python314.zip, the extension modules and the Visual C++ runtime
           beside them; its LICENSE.txt covers all of them), with the win_amd64 wheels in Lib/site-packages. Its python314._pth is left out: that file turns on isolated
           mode with safe_path, so Python would not put a script's own folder on sys.path, and the packer's tools import their neighbours that way (patch_nso.py's
           nso_asm, nro_collect.py's romfs). Without it Python finds its prefix by python314.zip beside python314.dll and takes the extension modules from its own folder
           (Modules/getpath.py, the Windows quirks); site adds Lib/site-packages. pack.cmd clears PYTHONHOME and PYTHONPATH and sets PYTHONNOUSERSITE for it
The wheels are unpacked as pip installs them (the .data folders' purelib and platlib into site-packages; their scripts, headers and data are not needed and left out).
Output: <out>/python/ (the runtime: bin/python3.14 on Linux, python.exe on Windows), <out>/licenses/ (CPython.txt, python-<library>.txt for what the Linux interpreter
links, lz4.txt, capstone.txt, fontTools.txt, fontTools-external.txt) and <out>/python.tsv (what it holds: the program's path, each piece's version).
--work <folder>: the downloads (dl/, kept for the next run; default <out>.work). --check: Linux runs the interpreter, with no environment of this machine's Python, on the
three modules and compression.zstd (a round trip each) and names every sys.path entry, all inside <out>; Windows (not run here) checks the files the runtime needs.
Exit status 0 when done, 1 on an error, 2 on a usage error."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

VERSION = '3.14.10'
PBS = 'https://github.com/astral-sh/python-build-standalone/releases/download/20261026/'
# name: (URL, SHA-256). python-build-standalone 20261026 (published 2026-10-26) and python.org's 3.14.10 (2026-10-25); the wheels as PyPI lists them
DOWNLOADS = {
    'cpython-3.14.10+20261026-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz':
        (PBS + 'cpython-3.14.10%2B20261026-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz', '4d3c16c59ed0802a3d1d34d8bce504b15ceeaff39d62dadecc6a9fa803412aed'),
    'cpython-3.14.10+20261026-x86_64-unknown-linux-gnu-pgo+lto-full.tar.zst':
        (PBS + 'cpython-3.14.10%2B20261026-x86_64-unknown-linux-gnu-pgo%2Blto-full.tar.zst', '4a2823d5deacb631e21b3c8387a1ec9be57e1b0ed37b7770e2e71cfe7b94eb94'),
    'python-3.14.10-embed-amd64.zip':
        ('https://www.python.org/ftp/python/3.14.10/python-3.14.10-embed-amd64.zip', '90905e64a840557d8c1bc4f5b74db3d7ee85e3ffb217611cac1c7813acea4159'),
    'lz4-4.4.5-cp314-cp314-manylinux2014_x86_64.manylinux_2_17_x86_64.manylinux_2_28_x86_64.whl':
        ('https://files.pythonhosted.org/packages/f1/01/d52c7b11eaa286d49dae619c0eec4aabc0bf3cda7a7467eb77c62c4471f3/'
         'lz4-4.4.5-cp314-cp314-manylinux2014_x86_64.manylinux_2_17_x86_64.manylinux_2_28_x86_64.whl', '0bba042ec5a61fa77c7e380351a61cb768277801240249841defd2ff0a10742f'),
    'lz4-4.4.5-cp314-cp314-win_amd64.whl':
        ('https://files.pythonhosted.org/packages/18/2c/8332080fd293f8337779a440b3a143f85e374311705d243439a3349b81ad/lz4-4.4.5-cp314-cp314-win_amd64.whl',
         '92159782a4502858a21e0079d77cdcaade23e8a5d252ddf46b0652604300d7be'),
    'capstone-5.0.9-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl':
        ('https://files.pythonhosted.org/packages/d1/39/17747862222bb062e86b501f1f148d5ff589b77908b080d30f7f085cbfb7/'
         'capstone-5.0.9-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl', '273fd8d747d2e35c88f91450be51a603ecfaafb00d96d9f315dcb8689c86193e'),
    'capstone-5.0.9-py3-none-win_amd64.whl':
        ('https://files.pythonhosted.org/packages/50/e6/6f06fdb6a9ed32b2f7cd9c036b92d5324112c3ef7080f2c71efc367d40dd/capstone-5.0.9-py3-none-win_amd64.whl',
         '732cedbbb56d42e723f14d7af6387f1454194a820b4b96b56d1e53f865ef85d0'),
    'fonttools-4.68.0-py3-none-any.whl':   # pure Python: the same wheel on both platforms (the compiled ones only make some of it faster)
        ('https://files.pythonhosted.org/packages/35/74/4ea331ecc65ba05a58b1e4e8ccd604c8a15565f876dfbf93fc2a2ea2e22b/fonttools-4.68.0-py3-none-any.whl',
         'd76f8fdcb6936f5e1045a0c9dd721a4bf11112e60d1e243bfa1d39f06a8b3104'),
}
PLATFORMS = {
    'linux': {'runtime': 'cpython-3.14.10+20261026-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz',
              'licences': 'cpython-3.14.10+20261026-x86_64-unknown-linux-gnu-pgo+lto-full.tar.zst',
              'wheels': ['lz4-4.4.5-cp314-cp314-manylinux2014_x86_64.manylinux_2_17_x86_64.manylinux_2_28_x86_64.whl',
                         'capstone-5.0.9-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl', 'fonttools-4.68.0-py3-none-any.whl'],
              'program': 'python/bin/python3.14', 'site': 'python/lib/python3.14/site-packages'},
    'windows': {'runtime': 'python-3.14.10-embed-amd64.zip',
                'wheels': ['lz4-4.4.5-cp314-cp314-win_amd64.whl', 'capstone-5.0.9-py3-none-win_amd64.whl', 'fonttools-4.68.0-py3-none-any.whl'],
                'program': 'python/python.exe', 'site': 'python/Lib/site-packages'},
}
# What the Linux runtime leaves out (paths under python/ of the archive): what a packer never runs
LINUX_LEFT_OUT = ('bin/', 'include/', 'share/', 'lib/pkgconfig/', 'lib/libpython', 'lib/libtcl', 'lib/tcl', 'lib/tk', 'lib/itcl', 'lib/thread',
                  'lib/python3.14/idlelib/', 'lib/python3.14/tkinter/', 'lib/python3.14/turtledemo/', 'lib/python3.14/turtle.py', 'lib/python3.14/ensurepip/',
                  'lib/python3.14/config-3.14-x86_64-linux-gnu/', 'lib/python3.14/lib-dynload/_tkinter.', 'lib/python3.14/site-packages/')
LINUX_KEPT = ('bin/python3.14', 'lib/python3.14/site-packages/README.txt')   # the interpreter itself, and site-packages' own marker
LINUX_LICENCES_LEFT_OUT = ('tcl', 'tix', 'libX11', 'libXau', 'libxcb')   # what tkinter links: left out with it
WINDOWS_LEFT_OUT = ('python314._pth',)
WHEEL_LICENCES = {'lz4': 'lz4', 'capstone': 'capstone', 'fonttools': 'fontTools'}   # the project's licence file name in licenses/


class PublishError(Exception):
    pass


def say(msg):
    print(f'publish_port_python: {msg}', flush=True)


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''): h.update(b)
    return h.hexdigest()


def fetch(name, dl):
    """dl/<name>, downloaded when it is not there with its SHA-256; a file of another SHA-256 is fetched again, and a download of another one stops the run"""
    url, want = DOWNLOADS[name]
    p = dl / name
    if p.is_file() and sha256(p) == want: return p
    say(f'download: {url}')
    part = p.with_name(p.name + '.part')
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'nxnative-publish'}), timeout=300) as r, open(part, 'wb') as f: shutil.copyfileobj(r, f)
    except OSError as e: raise PublishError(f'download of {url} failed: {e}') from e
    got = sha256(part)
    if got != want:
        part.unlink()
        raise PublishError(f'{url}: SHA-256 is {got}, the pinned one {want} (a changed file: check it, then pin it again)')
    os.replace(part, p)
    return p


def put(path, data, mode):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data); os.chmod(path, mode)


def linux_runtime(archive, out):
    """python-build-standalone's install_only_stripped tree into out/python, without LINUX_LEFT_OUT and without symbolic links. Returns the files written"""
    n, links = 0, []
    with tarfile.open(archive, 'r:gz') as tar:
        for m in tar:
            if not m.name.startswith('python/'): raise PublishError(f'{archive.name}: {m.name} is outside python/')
            rel = m.name[len('python/'):]
            if m.issym() or m.islnk(): links.append(rel); continue
            if not m.isfile() or (rel.startswith(LINUX_LEFT_OUT) and rel not in LINUX_KEPT): continue
            if '..' in Path(rel).parts or rel.startswith('/'): raise PublishError(f'{archive.name}: {m.name} leaves its folder')
            put(out / 'python' / rel, tar.extractfile(m).read(), 0o755 if m.mode & 0o111 else 0o644); n += 1
    if not (out / 'python/bin/python3.14').is_file(): raise PublishError(f'{archive.name} has no python/bin/python3.14')
    say(f'runtime: {n} files of {archive.name}; left out: {len(links)} symbolic links ({", ".join(l for l in links if "terminfo" not in l)} and terminfo\'s), '
        + ', '.join(LINUX_LEFT_OUT))
    return n


def windows_runtime(archive, out):
    """python.org's embeddable package into out/python, without its python314._pth"""
    n = 0
    with zipfile.ZipFile(archive) as z:
        for i in z.infolist():
            if i.is_dir() or i.filename in WINDOWS_LEFT_OUT: continue
            if '..' in Path(i.filename).parts or i.filename.startswith('/'): raise PublishError(f'{archive.name}: {i.filename} leaves its folder')
            put(out / 'python' / i.filename, z.read(i), 0o755 if i.filename.endswith(('.exe', '.dll', '.pyd')) else 0o644); n += 1
    for need in ('python.exe', 'python314.dll', 'python314.zip'):
        if not (out / 'python' / need).is_file(): raise PublishError(f'{archive.name} has no {need}')
    (out / 'python/Lib/site-packages').mkdir(parents=True, exist_ok=True)
    say(f'runtime: {n} files of {archive.name}; left out: {", ".join(WINDOWS_LEFT_OUT)} (isolated mode would keep a script\'s folder off sys.path)')
    return n


def install_wheel(wheel, site):
    """A wheel unpacked into site as pip installs it: its files, the .data folder's purelib and platlib merged in (scripts, headers, data left out). Returns
    (project, version, [licence files in its dist-info])"""
    project, version = wheel.name.split('-')[:2]
    data = f'{project}-{version}.data/'
    licences = []
    with zipfile.ZipFile(wheel) as z:
        for i in z.infolist():
            name = i.filename
            if i.is_dir(): continue
            if name.startswith(data):
                kind, _, rest = name[len(data):].partition('/')
                if kind not in ('purelib', 'platlib'): continue
                name = rest
            if '..' in Path(name).parts or name.startswith('/'): raise PublishError(f'{wheel.name}: {i.filename} leaves its folder')
            mode = 0o755 if (i.external_attr >> 16) & 0o111 or name.endswith(('.so', '.pyd', '.dll')) else 0o644
            put(site / name, z.read(i), mode)
            if '.dist-info/' in name and Path(name).name.upper().startswith(('LICEN', 'COPYING')): licences.append(site / name)
    if not licences: raise PublishError(f'{wheel.name} holds no licence file in its .dist-info')
    return project, version, sorted(licences)


def licence(path, head, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(f'{head}\n\n{text}', encoding='utf-8', newline='\n')


def linux_licences(full, out):
    """python/licenses/ of the full archive (zstd: read with the interpreter just unpacked, whose tarfile reads .tar.zst) into out/licenses: CPython.txt and python-<name>.txt"""
    code = ('import json, sys, tarfile\nrows = {}\nwith tarfile.open(sys.argv[1], "r:zst") as t:\n    for m in t:\n'
            '        if m.isfile() and m.name.startswith("python/licenses/"): rows[m.name[16:]] = t.extractfile(m).read().decode("utf-8", "replace")\n'
            'json.dump(rows, sys.stdout)\n')
    r = subprocess.run([str(out / 'python/bin/python3.14'), '-I', '-c', code, str(full)], capture_output=True, text=True, env=clean_env())
    if r.returncode: raise PublishError(f'reading the licences of {full.name} failed: {r.stderr.strip()[-400:]}')
    rows, written = json.loads(r.stdout), []
    for name, text in sorted(rows.items()):
        lib = name.removeprefix('LICENSE.').removesuffix('.txt')
        if lib in LINUX_LICENCES_LEFT_OUT: continue
        dst = 'CPython.txt' if lib == 'cpython' else f'python-{lib}.txt'
        licence(out / 'licenses' / dst, f'{"CPython " + VERSION if lib == "cpython" else lib}: python-build-standalone 20261026\'s python/licenses/{name} '
                f'({full.name}), the licence of {"the interpreter" if lib == "cpython" else "a library linked into the interpreter"} in python/', text)
        written.append(dst)
    if 'CPython.txt' not in written: raise PublishError(f'{full.name} has no python/licenses/LICENSE.cpython.txt')
    return written


def clean_env(**more):   # an environment with nothing of this machine's Python: no PYTHON* variable, no user site-packages, a home of its own
    env = {k: v for k, v in os.environ.items() if not k.startswith('PYTHON')}
    env.update(PYTHONNOUSERSITE='1', PYTHONDONTWRITEBYTECODE='1', **more)
    return env


PROBE = '''import sys, os
import lz4, lz4.block, capstone, fontTools, fontTools.ttLib, compression.zstd
data = bytes(range(256)) * 64
assert lz4.block.decompress(lz4.block.compress(data)) == data
assert compression.zstd.decompress(compression.zstd.compress(data)) == data
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
assert [i.mnemonic for i in md.disasm(bytes.fromhex("1f2003d5c0035fd6"), 0)] == ["nop", "ret"]
print("python", sys.version.split()[0], "prefix", sys.prefix)
print("lz4", lz4.__version__, "capstone", capstone.__version__, "fontTools", fontTools.version, "zstd", compression.zstd.zstd_version)
for p in sys.path: print("path", p)
'''


def check(platform, out):
    """Linux: PROBE through the interpreter, with clean_env and from another folder, and every sys.path entry inside out; Windows: the files the runtime needs"""
    plat = PLATFORMS[platform]
    if platform == 'windows':
        need = ['python/python.exe', 'python/python314.dll', 'python/python314.zip', 'python/vcruntime140.dll', 'python/_ctypes.pyd', 'python/_zstd.pyd',
                'python/Lib/site-packages/lz4/block/_block.cp314-win_amd64.pyd', 'python/Lib/site-packages/capstone/lib/capstone.dll',
                'python/Lib/site-packages/fontTools/ttLib/__init__.py']
        missing = [n for n in need if not (out / n).is_file()]
        if missing: raise PublishError('the Windows runtime lacks ' + ', '.join(missing))
        if (out / 'python/python314._pth').exists(): raise PublishError('python/python314._pth is there: Python would run isolated, without a script\'s folder on sys.path')
        say(f'check: {len(need)} files the Windows runtime needs are there, no python314._pth (python.exe is not run here: a Windows program)')
        return
    if sys.platform != 'linux': raise PublishError('the Linux runtime is checked on Linux')
    home = out.parent / (out.name + '.check')
    shutil.rmtree(home, ignore_errors=True); home.mkdir()
    try:
        r = subprocess.run([str(out / plat['program']), '-c', PROBE], capture_output=True, text=True, cwd=home, env=clean_env(HOME=str(home)), timeout=120)
    finally: shutil.rmtree(home, ignore_errors=True)
    if r.returncode: raise PublishError(f'{plat["program"]} -c <probe>: exit {r.returncode}: {(r.stdout + r.stderr).strip()[-600:]}')
    paths = [l[5:] for l in r.stdout.splitlines() if l.startswith('path ')]
    outside = [p for p in paths if p and not Path(p).resolve().is_relative_to(out.resolve())]
    if outside: raise PublishError(f'sys.path has entries outside {out}: {outside}')
    for l in r.stdout.splitlines():
        if not l.startswith('path '): say(f'check: {l}')
    say(f'check: sys.path has {len(paths)} entries, every one inside {out} or the current folder')


def main(argv=None):
    ap = argparse.ArgumentParser(description='The packer\'s own Python for the port\'s bundles (scripts/package_port.py --python).')
    ap.add_argument('platform', choices=sorted(PLATFORMS))
    ap.add_argument('out', type=Path)
    ap.add_argument('--work', type=Path, help='the downloads (dl/; default <out>.work)')
    ap.add_argument('--check', action='store_true', help='check the runtime (Linux: run it)')
    a = ap.parse_args(argv)
    try:
        plat, out = PLATFORMS[a.platform], a.out.resolve()
        dl = (a.work.resolve() if a.work else out.parent / (out.name + '.work')) / 'dl'
        dl.mkdir(parents=True, exist_ok=True)
        files = {n: fetch(n, dl) for n in [plat['runtime'], *plat['wheels'], *([plat['licences']] if 'licences' in plat else [])]}
        for d in ('python', 'licenses'): shutil.rmtree(out / d, ignore_errors=True)
        (out / 'python.tsv').unlink(missing_ok=True)
        out.mkdir(parents=True, exist_ok=True)
        if a.platform == 'linux':
            linux_runtime(files[plat['runtime']], out)
            written = linux_licences(files[plat['licences']], out)
        else:
            windows_runtime(files[plat['runtime']], out)
            with zipfile.ZipFile(files[plat['runtime']]) as z:
                licence(out / 'licenses/CPython.txt', f'CPython {VERSION}: python.org\'s {plat["runtime"]} (LICENSE.txt), the licence of python/ and of what it links '
                        '(OpenSSL, libffi, SQLite, bzip2, xz, zstd, the Visual C++ runtime)', z.read('LICENSE.txt').decode('utf-8', 'replace'))
            written = ['CPython.txt']
        site, rows = out / plat['site'], [('program', plat['program']), ('python', VERSION)]
        for w in plat['wheels']:
            project, version, lic = install_wheel(files[w], site)
            base = WHEEL_LICENCES[project]
            for i, p in enumerate(lic):
                dst = base if i == 0 else f'{base}-{p.name.upper().removeprefix("LICENSE").strip(".").lower() or i}'   # LICENSE.external: fontTools-external.txt
                licence(out / 'licenses' / f'{dst}.txt', f'{project} {version} ({w}, PyPI), its {p.name}: the licence of a module in python/', p.read_text('utf-8', 'replace'))
                written.append(f'{dst}.txt')
            rows.append((project, version))
        (out / 'python.tsv').write_text('# the packer\'s Python of dm/tools/publish_port_python.py: the program (its path in the bundle) and the version of each piece\n'
                                        + ''.join(f'{k}\t{v}\n' for k, v in rows), encoding='utf-8', newline='\n')
        size = sum(f.stat().st_size for f in (out / 'python').rglob('*') if f.is_file())
        say(f'{out / "python"}: {sum(1 for f in (out / "python").rglob("*") if f.is_file())} files, {size / 1e6:.1f} MB; ' + ', '.join(f'{k} {v}' for k, v in rows[1:])
            + f'; licences: {", ".join(written)}')
        if a.check: check(a.platform, out)
    except PublishError as e:
        print(f'publish_port_python: [RED] {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
