#!/usr/bin/env python3
# make_package.py <out.zip> <kit folder> <spirv-cross.exe>: the folder a Windows PC needs, as a zip. Run from the repository; it takes HEAD. DM_PKG_TREE=<tree> takes that tree instead (git write-tree: a merge not yet committed). A game's own profile goes in whole, the others as the build
# reads them (their game.tsv and patches.py). It holds no game files or keys, and refuses a file that looks like one.
import io, json, os, re, shutil, subprocess, sys, tarfile, tempfile, zipfile
out, kit, spirv = sys.argv[1:4]; HERE = os.path.dirname(os.path.abspath(__file__)); game = json.load(open(os.path.join(kit, 'kit.json'), encoding='utf-8')).get('game')
PATHS = 'scripts dm/tools dm/gfx/tools dm/gfx/nvn/dmsh_abi.h dm/gfx/nvn/dmsh_chain.h dm/mac dm/ios/tools dm/ios/Sources/LauncherSettings.swift dm/ios/Sources/LauncherRow.swift dm/host/romfs_image.h dm/host/save_sync.c dm/host/save_sync.h dm/games dm/third_party/ryujinx/src/Ryujinx.Graphics.Shader'.split()
def keep(n):
    m = re.match(r'dm/games/([^/]+)/(.+)', n)
    return not m or not game or m[1] == game or m[2] in ('game.tsv', 'patches.py')   # a kit without a game: every profile whole, for a queue of games
def patched(n, b):   # until these move into the tools themselves: the lock check reports (Windows' tools differ by design), and the image reader takes Windows paths
    if n == 'dm/gfx/tools/tool_lock.py':
        s = b.decode(); a = "if __name__ == '__main__':"; assert s.count(a) == 1
        return s.replace(a, "_real_check = check_tools\ndef check_tools():   # the Windows package's copy: reports the differences and goes on\n    for b in _real_check(): print('windows build, tools differ from the lock: ' + b, file=sys.stderr, flush=True)\n    return []\n\n" + a).encode()
    if n == 'dm/tools/romfs.py':
        s = b.decode(); a = "    parts = p.split('/')\n"; assert s.count(a) == 1
        return s.replace(a, "    parts = p.replace(_os.sep, '/').split('/')   # the Windows package's copy\n").encode()
    return b
data = subprocess.run(['git', 'archive', os.environ.get('DM_PKG_TREE', 'HEAD'), *PATHS], capture_output=True, check=True).stdout
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
    with tarfile.open(fileobj=io.BytesIO(data)) as t:
        for m in t.getmembers():
            if m.isfile() and keep(m.name): z.writestr('repo/' + m.name, patched(m.name, t.extractfile(m).read()))
    for n in ('run_all.py', 'RUN_BUILD.bat'): z.write(os.path.join(HERE, n), n)
    z.write(os.path.join(HERE, 'PACKAGE_README.txt'), 'README.txt')
    for n in sorted(os.listdir(os.path.join(HERE, 'winshim'))):
        if n.endswith('.py'): z.write(os.path.join(HERE, 'winshim', n), 'winshim/' + n)
    for r, _, fs in os.walk(kit):
        for n in fs:
            if not n.startswith('._'): q = os.path.join(r, n); z.write(q, 'kit/' + os.path.relpath(q, kit))
    z.write(spirv, 'tools/spirv-cross.exe')
    z.writestr('game/PUT_NSP_HERE.txt', 'Put the game .nsp here.\n'); z.writestr('keys/PUT_KEYS_HERE.txt', 'Put prod.keys here.\n')
    bad = [n for n in z.namelist() if n.lower().endswith(('.nsp', '.xci', '.keys', 'romfs.img', '.patched.bin'))]
if bad: os.unlink(out); sys.exit(f'[RED] game data in the package: {bad[:3]}')
print(out, os.path.getsize(out) >> 20, 'MB')
