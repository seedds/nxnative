"""The port's bundle (scripts/package_port.py; docs/development.md, "The port's bundles"): assembled from a fake build tree (stand-ins for nxlauncher, nxhost-run and nxaot,
SDL3's and LLVM's licences where the build trees keep them) and a few of this checkout's files, the missing-file errors, the archives' determinism (two runs give the same bytes,
whatever the inputs' times), and the bundle's own check where it can run (Linux: the stand-ins are run as the real programs would be; the host's stand-in answers --game and
--profile with dm/host/game_rt.c's profile line, from a table of profiles it is made with). No Docker, game files or keys."""
import importlib.util
import io
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock
import zipfile

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('package_port', REPO / 'scripts/package_port.py')
pp = importlib.util.module_from_spec(spec); spec.loader.exec_module(pp)
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import dmsh_chain, dmsh_chain_vk   # noqa: E402

GAMES = ('010015100B514000', '0100000000010000')   # two profiles of dm/games
# A few of the packer's files instead of all of them (about 220 MB): the packer and what its --help imports, the chain tools with every file their versions hash and the headers,
# the two profiles' game.tsv and game.py (which writes their files for the host's check); the bundle's check runs on these as on the whole set
CHAIN = sorted({os.path.normpath(f'dm/gfx/{f}') for f in dmsh_chain.FILES + dmsh_chain_vk.FILES})
TREES = ('scripts/build_ipa_from_nsp.py', 'dm/tools/romfs.py', 'dm/gfx/nvn/dmsh_chain.h', 'dm/gfx/nvn/dmsh_chain_vk.h', *CHAIN, 'dm/games/game.py',
         *(f'dm/games/{g}/game.tsv' for g in GAMES))
# a stand-in for nxhost-run (dm/host/game_rt.c, main_x86.c): --game <id> of its table (@TABLE@: the profiles it was built with) or --profile <file> prints the profile line, the
# name from the bundle's game.tsv or the file's name row; an unknown title ID is refused with a [RED] line; then the usage (no module folder), status 2
HOST = '''#!/bin/sh
here=$(cd "$(dirname "$0")" && pwd); tab=$(printf '\\t')
case $1 in
  --game) case " @TABLE@ " in *" $2 "*) ;; *) echo "[dm] [RED] --game $2: no such game profile among the ones built into this program" >&2; exit 2 ;; esac
          echo "[dm] game profile $2 ($(sed -n "s/^name$tab//p" "$here/../dm/games/$2/game.tsv")): the table built into the program (--game)" >&2 ;;
  --profile) echo "[dm] game profile $(sed -n "s/^id$tab//p" "$2") ($(sed -n "s/^name$tab//p" "$2")): the file $2 (--profile)" >&2 ;;
esac
echo "usage: nxhost-run <dir> [module...] [-- game arguments...]" >&2
exit 2
'''
LAUNCHER = '''#!/bin/sh
# a stand-in for nxlauncher: --list prints its header line, as on an empty games folder
if [ "$1" = --list ]; then echo "nxlauncher: 0 games in $2" >&2; printf '# title_id\\ttitle\\tversion\\tpacks\\tlast_played\\tfolder\\tproblem\\tbetter_pack\\n'; exit 0; fi
exit 2
'''
NXAOT = '''#!/bin/sh
# a stand-in for nxaot: nxaot version
if [ "$1" = version ]; then printf 'llvm\\t18.1.3\\ndynarmic\\ta466015\\n'; exit 0; fi
exit 2
'''
# a stand-in for the bundle's Python (dm/tools/publish_port_python.py): -c (the check's probe) prints what the real one prints, its sys.path in the bundle (@MORE@: other lines);
# anything else runs this machine's Python on it (the packer's --help, dmsh_chain_vk.py --check-tools). No external command: the check runs it with PATH holding only bin/
PYTHON = '''#!/bin/sh
here=${0%/*}/..
if [ "$1" = -c ]; then echo "python 3.14.10, lz4 4.4.5, capstone 5.0.7, fontTools 4.68.0, zstd 1.5.7"; echo "path $here/lib/python3.14"; @MORE@ exit 0; fi
exec @PY@ -I "$@"
'''
PE = b'MZ' + bytes(0x3a) + (0x40).to_bytes(4, 'little') + b'PE\0\0' + (0x8664).to_bytes(2, 'little') + bytes(18) + (0x20b).to_bytes(2, 'little') + bytes(0x100)   # a PE32+ x86-64 header


def flat(text):   # README.txt's text with its line breaks and indents as single spaces (a sentence is found whatever the wrapping)
    return ' '.join(text.split())


class Bundle(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()).resolve(); self.addCleanup(shutil.rmtree, self.tmp)
        p = mock.patch.object(pp, 'PACKER_TREES', TREES); p.start(); self.addCleanup(p.stop)
        self.build, self.aot = self.tmp / 'build/linux', self.tmp / 'build/aot'
        self.fake_tree(self.build, '')
        (self.aot / 'licenses').mkdir(parents=True)
        self.write(self.aot / 'nxaot', NXAOT, 0o755); self.write(self.aot / 'licenses/LLVM.txt', 'LLVM licence\n')

    def write(self, path, text, mode=0o644):
        path.parent.mkdir(parents=True, exist_ok=True); path.write_text(text); path.chmod(mode)

    def fake_tree(self, build, exe, table=GAMES):
        """A host tree as the CI leaves it: nxhost-run (its table: the profiles of table), the launcher, SDL3's licence"""
        self.write(build / 'CMakeCache.txt', f'CMAKE_BUILD_TYPE:STRING=\nGAME:STRING={GAMES[0]}\n')
        self.write(build / f'nxhost-run{exe}', HOST.replace('@TABLE@', ' '.join(table)) if not exe else 'MZ host', 0o755)
        self.write(build / f'nxlauncher{exe}', LAUNCHER if not exe else 'MZ launcher', 0o755)
        self.write(build / '_deps/sdl3-src/LICENSE.txt', 'SDL3 licence\n')

    def package(self, *args, platform='linux', out=None, build=None, aot=True, said=None):   # said: a list that gets what the script says
        out = out or self.tmp / 'out' / f'nxnative-{platform}-x86_64'
        argv = ['--platform', platform, '--build', str(build or self.build), '--out', str(out)] + (['--aot', str(self.aot)] if aot else []) + list(args)
        err = io.StringIO()
        with mock.patch.object(sys, 'stderr', err), mock.patch.object(pp, 'say', said.append if said is not None else lambda m: None):
            rc = pp.main(argv)
        return rc, err.getvalue(), out

    def test_assembly(self):
        rc, err, out = self.package()
        self.assertEqual((rc, err), (0, ''))
        files = dict(pp.bundle_files(out))
        for rel in ('nxlauncher', 'bin/nxhost-run', 'bin/nxaot', 'pack.sh'):
            self.assertEqual(files.pop(rel), 0o755, rel)
        self.assertEqual((out / 'bin/nxhost-run').read_bytes(), (self.build / 'nxhost-run').read_bytes())   # the one host, whatever the tree's GAME
        self.assertFalse((out / 'hosts').exists())
        self.assertEqual(files.pop('scripts/build_ipa_from_nsp.py'), 0o755)   # git's executable bit
        for rel in ('README.txt', 'VERSION', 'licenses/nxnative.txt', 'licenses/SDL3.txt', 'licenses/LLVM.txt', 'licenses/imgui.txt', 'licenses/dynarmic.txt',
                    'licenses/dynarmic-fmt.txt', 'licenses/libvpx-PATENTS.txt', 'licenses/fsr1.txt', 'dm/tools/romfs.py', 'dm/gfx/nvn/dmsh_chain_vk.h', f'dm/games/{GAMES[0]}/game.tsv'):
            self.assertEqual(files.pop(rel), 0o644, rel)
        self.assertEqual((out / 'licenses/SDL3.txt').read_text(), 'SDL3 licence\n')
        self.assertEqual((out / 'licenses/nxnative.txt').read_bytes(), (REPO / 'LICENSE').read_bytes())
        self.assertFalse([r for r in files if not r.startswith(('licenses/', 'dm/'))], files)   # nothing else at the top
        sha = subprocess.run(['git', '-C', str(REPO), 'log', '-1', '--format=%h'], capture_output=True, text=True).stdout.strip()
        version = (out / 'VERSION').read_text().splitlines()
        self.assertTrue(version[0].startswith(f'nxnative {sha}'), version)
        self.assertIn('host bin/nxhost-run', version); self.assertIn('profiles 2', version); self.assertIn('platform linux-x86_64', version)
        self.assertFalse([l for l in version if l.startswith('hosts')])
        self.assertIn('python no', version); self.assertIn('spirv no', version)
        readme = (out / 'README.txt').read_text()
        self.assertIn('./pack.sh "<the game\'s NSP folder>" -k "<your prod.keys>"', readme); self.assertIn('pip install lz4 capstone fonttools backports.zstd', flat(readme))
        self.assertIn('which this folder does not carry: without them the packer reports the NVN shaders as not run', flat(readme))   # no --spirv: said
        self.assertNotIn('nothing to install', readme); self.assertNotIn('python/', readme)
        self.assertRegex(readme, r'\n  bin/nxhost-run +the host program the launcher starts, one for every game'); self.assertIn('Game profiles in this folder: 2.', readme)
        self.assertNotIn('hosts/', readme + (out / 'pack.sh').read_text())
        self.assertIn('exec "${PYTHON:-python3}" "$here/scripts/build_ipa_from_nsp.py" --platform linux "$@"', (out / 'pack.sh').read_text())
        # a second run replaces the earlier bundle; a folder that is not one is refused
        (out / 'stale.txt').write_text('old')
        self.assertEqual(self.package()[0], 0); self.assertFalse((out / 'stale.txt').exists())
        other = self.tmp / 'mine'; other.mkdir(); (other / 'notes.txt').write_text('mine')
        rc, err, _ = self.package(out=other)
        self.assertEqual(rc, 1); self.assertIn('is not an earlier bundle', err); self.assertTrue((other / 'notes.txt').is_file())

    def test_missing_files(self):
        def fails(why, **kw):
            rc, err, _ = self.package(**kw)
            self.assertEqual(rc, 1, err); self.assertRegex(err, why)
        (self.build / 'nxlauncher').unlink()
        fails(r'missing .*nxlauncher: the launcher')
        self.write(self.build / 'nxlauncher', LAUNCHER, 0o755)
        (self.build / '_deps/sdl3-src/LICENSE.txt').unlink()
        fails(r'missing .*_deps/sdl3-src/LICENSE.txt: SDL3')
        self.write(self.build / '_deps/sdl3-src/LICENSE.txt', 'SDL3 licence\n')
        (self.aot / 'licenses/LLVM.txt').unlink()
        fails(r'missing .*licenses/LLVM.txt: the licence of the LLVM')
        self.write(self.aot / 'licenses/LLVM.txt', 'LLVM licence\n')
        fails(r'--aot is required on Linux', aot=False)
        (self.aot / 'nxaot').unlink()
        fails(r'missing .*nxaot: the recompiler')
        self.write(self.aot / 'nxaot', NXAOT, 0o755)
        (self.build / 'CMakeCache.txt').unlink()
        self.assertEqual(self.package()[0], 0)   # the tree's GAME is only the host's default: not needed
        (self.build / 'nxhost-run').unlink()
        fails(r'no host in .*: no nxhost-run \(cmake --build .* --target nxhost-run: one program for every game profile\)$')
        # a tree of the layout before the one host (hosts/<title ID>/nxhost-run): named in the error, not taken
        self.write(self.build / f'hosts/{GAMES[1]}/nxhost-run', HOST, 0o755)
        fails(r'no host in .*: no nxhost-run .*hosts/ holds the layout before it \(one host per game profile\), which a bundle no longer takes')
        self.assertEqual(self.package(build=self.tmp / 'none')[0], 1)

    def test_archives_are_deterministic(self):
        def both(platform, **kw):
            got = []
            for i, parent in enumerate(('a', 'b')):
                for p in (self.build, self.aot):   # other times on the inputs each run: they do not reach the archive
                    for f in p.rglob('*'): os.utime(f, (1_000_000_000 + i * 12345, 1_000_000_000 + i * 12345))
                rc, err, out = self.package('--archive', platform=platform, out=self.tmp / parent / f'nxnative-{platform}-x86_64', **kw)
                self.assertEqual((rc, err), (0, ''))
                got.append(out.parent / (out.name + pp.PLATFORMS[platform]['archive']))
            self.assertEqual(got[0].read_bytes(), got[1].read_bytes())
            return got[0], out
        tgz, out = both('linux')
        ts = int(subprocess.run(['git', '-C', str(REPO), 'log', '-1', '--format=%ct'], capture_output=True, text=True).stdout)
        with tarfile.open(tgz) as tar:
            members = tar.getmembers()
            names = [m.name for m in members]
            self.assertEqual(names, sorted(names)); self.assertEqual(names[0], 'nxnative-linux-x86_64')
            self.assertEqual({(m.mtime, m.uid, m.gid, m.uname, m.gname) for m in members}, {(ts, 0, 0, '', '')})
            modes = {m.name: m.mode for m in members}
            self.assertEqual(modes['nxnative-linux-x86_64/nxlauncher'], 0o755); self.assertEqual(modes['nxnative-linux-x86_64/bin/nxaot'], 0o755)
            self.assertEqual(modes['nxnative-linux-x86_64/README.txt'], 0o644); self.assertEqual(modes['nxnative-linux-x86_64/bin'], 0o755)
            self.assertEqual(modes['nxnative-linux-x86_64/bin/nxhost-run'], 0o755)
            self.assertFalse([m for m in members if not (m.isfile() or m.isdir())])   # no link
            self.assertEqual({n[len('nxnative-linux-x86_64/'):] for n in names if tar.getmember(n).isfile()}, {r for r, _ in pp.bundle_files(out)})
        # Windows: the .exe stand-ins, no nxaot, a zip
        win = self.tmp / 'build/win64'; self.fake_tree(win, '.exe')
        self.build = win
        zp, out = both('windows', aot=False)
        with zipfile.ZipFile(zp) as z:
            infos = z.infolist(); names = [i.filename for i in infos]
            self.assertEqual(names, sorted(names))
            self.assertIn('nxnative-windows-x86_64/nxlauncher.exe', names); self.assertIn('nxnative-windows-x86_64/bin/nxhost-run.exe', names)
            self.assertFalse([n for n in names if '/hosts/' in n])
            self.assertIn(b'host bin/nxhost-run.exe\r\n', z.read('nxnative-windows-x86_64/VERSION'))
            self.assertIn(b'bin\\nxhost-run.exe', z.read('nxnative-windows-x86_64/pack.cmd'))
            self.assertIn('nxnative-windows-x86_64/pack.cmd', names); self.assertNotIn('nxnative-windows-x86_64/bin/nxaot.exe', names)
            self.assertEqual(len({i.date_time for i in infos}), 1)
            self.assertIn(b'\r\n', z.read('nxnative-windows-x86_64/pack.cmd')); self.assertIn(b'nxaot no', z.read('nxnative-windows-x86_64/VERSION'))
            self.assertIn(b'nxaot is\r\nrem a Linux program', z.read('nxnative-windows-x86_64/pack.cmd'))   # without --aot, the text of a bundle without nxaot
            self.assertIn(b'The recompiler nxaot is a Linux program: this folder has none', z.read('nxnative-windows-x86_64/README.txt'))

    def test_windows_nxaot(self):   # --aot on Windows: bin/nxaot.exe and LLVM's licence in the bundle, pack.cmd and README.txt say the packer runs it
        win, aot = self.tmp / 'build/win64', self.tmp / 'build/aot-win64'
        self.fake_tree(win, '.exe')
        self.write(aot / 'nxaot.exe', 'MZ nxaot', 0o755); self.write(aot / 'licenses/LLVM.txt', 'LLVM licence\n')
        self.aot = aot
        rc, err, out = self.package('--archive', platform='windows', build=win)
        self.assertEqual((rc, err), (0, ''))
        files = dict(pp.bundle_files(out))
        self.assertEqual(files['bin/nxaot.exe'], 0o755); self.assertEqual((out / 'bin/nxaot.exe').read_text(), 'MZ nxaot')
        self.assertEqual((out / 'licenses/LLVM.txt').read_text(), 'LLVM licence\n')
        cmd, readme = (out / 'pack.cmd').read_bytes(), (out / 'README.txt').read_text()
        self.assertIn(b"with the bundle's recompiler (bin\\nxaot.exe)", cmd); self.assertNotIn(b'Linux program', cmd); self.assertIn(b'\r\n', cmd)
        self.assertNotIn('Linux program', readme); self.assertRegex(readme, r'\n  bin/nxaot\.exe +the recompiler the packer runs\r?\n')
        self.assertIn('nxaot yes', (out / 'VERSION').read_text())
        with zipfile.ZipFile(out.parent / (out.name + '.zip')) as z: self.assertIn('nxnative-windows-x86_64/bin/nxaot.exe', z.namelist())
        # the licence is required with nxaot.exe, and the error says where it comes from
        (aot / 'licenses/LLVM.txt').unlink()
        rc, err, _ = self.package(platform='windows', build=win)
        self.assertEqual(rc, 1); self.assertRegex(err, r'missing .*licenses/LLVM.txt: .*LICENSE.TXT from the prefix dm/tools/aot/tools/build_llvm_win64.sh built')
        (aot / 'nxaot.exe').unlink()
        rc, err, _ = self.package(platform='windows', build=win)
        self.assertEqual(rc, 1); self.assertRegex(err, r'missing .*nxaot.exe: the recompiler')

    def tools(self, exe):   # stand-ins for dm/tools/publish_port_tools.sh's output: nx_extract (its usage, exit 2), dmsh_xlate_vk (--source-sha), their licences
        tools = self.tmp / f'build/port_tools/{"win-x64" if exe else "linux-x64"}'
        self.write(tools / f'nx_extract{exe}', 'MZ nx_extract' if exe else '#!/bin/sh\necho "usage: nx_extract <prod.keys> <base.nsp> <update.nsp|-> <out dir>" >&2; exit 2\n', 0o755)
        self.write(tools / f'dmsh_xlate_vk{exe}', 'MZ dmsh_xlate_vk' if exe else '#!/bin/sh\n[ "$1" = --source-sha ] && { echo 0123456789abcdef; exit 0; }; exit 2\n', 0o755)
        for n in ('dotnet-runtime.txt', 'dotnet-runtime-THIRD-PARTY-NOTICES.txt', 'LibHac.txt', 'ryujinx.txt'): self.write(tools / 'licenses' / n, f'{n} of the tools\n')
        return tools

    @unittest.skipUnless(sys.platform == 'linux', 'the Linux bundle\'s check runs its programs')
    def test_tools(self):   # --tools: bin/nx_extract and bin/dmsh_xlate_vk with their licences; README.txt asks for no .NET; the check runs them and holds the translator to the lock
        tools = self.tools('')
        rc, err, out = self.package('--tools', str(tools))
        self.assertEqual((rc, err), (0, ''))
        files = dict(pp.bundle_files(out))
        self.assertEqual((files['bin/nx_extract'], files['bin/dmsh_xlate_vk']), (0o755, 0o755))
        self.assertEqual((out / 'licenses/dotnet-runtime.txt').read_text(), 'dotnet-runtime.txt of the tools\n')
        self.assertEqual((out / 'licenses/LibHac.txt').read_text(), 'LibHac.txt of the tools\n')
        self.assertEqual((out / 'licenses/ryujinx.txt').read_bytes(), (REPO / 'dm/third_party/ryujinx/LICENSE.txt').read_bytes())   # the name was taken: the vendored copy's
        readme = flat((out / 'README.txt').read_text())
        self.assertNotIn('.NET', readme.replace('none of them needs .NET', '')); self.assertNotIn('Vulkan shader translator, which this folder does not carry', readme)
        self.assertIn("The packer's extraction tool and Vulkan shader translator are in bin/, and none of them needs .NET.", readme)
        self.assertIn('SPIRV-Tools, glslang and SPIRV-Cross of the versions dm/gfx/tools/dmsh_chain_vk.lock records, which this folder does not carry', readme)
        self.assertRegex((out / 'README.txt').read_text(), r'\n  bin/nx_extract +the NSP extraction tool the packer runs\n  bin/dmsh_xlate_vk +the Vulkan shader translator the packer runs for NVN games\n')
        # without --tools: the .NET SDK and the translator are asked for
        profiles = list(GAMES)
        for platform in ('linux', 'windows'):
            for nxaot in (False, True):
                self.assertEqual(pp.readme(platform, 'abc1234', '2026-11-15', profiles, nxaot), pp.readme(platform, 'abc1234', '2026-11-15', profiles, nxaot, False))
        self.assertIn('and the .NET SDK 10 (the packer builds its extraction tool, nx_extract, with it on the first pack). NVN games also need the Vulkan shader translator, '
                      'which this folder does not carry', flat(pp.readme('linux', 'abc1234', '2026-11-15', profiles, True)))
        # the check: the stand-ins run, and the translator is not the one the bundle's lock fingerprints
        rc, err, out = self.package('--tools', str(tools), '--check')
        self.assertEqual(rc, 1); self.assertIn('dm/gfx/tools/dmsh_chain_vk.py: exit 1', err); self.assertIn('does not match the lock: dmsh_xlate_vk_linux-x64', err)
        # with the stand-in's fingerprint in the bundle's lock (and the bundle's header written again for it) the check passes
        lock = out / 'dm/gfx/tools/dmsh_chain_vk.lock'
        lock.write_text(lock.read_text().replace(dmsh_chain_vk.lock()['dmsh_xlate_vk_linux-x64'], dmsh_chain.sha16(out / 'bin/dmsh_xlate_vk')))
        subprocess.run([sys.executable, '-I', out / 'dm/gfx/tools/dmsh_chain_vk.py', '--header'], check=True, capture_output=True)
        with mock.patch.object(pp, 'say', lambda m: None): self.assertEqual(pp.check('linux', out, [self.build, self.aot, tools]), [])
        # a tool or the runtime's licence missing
        (tools / 'licenses/dotnet-runtime.txt').unlink()
        rc, err, _ = self.package('--tools', str(tools)); self.assertEqual(rc, 1); self.assertRegex(err, r'missing .*dotnet-runtime.txt: the licence of the .NET runtime')
        (tools / 'nx_extract').unlink()
        rc, err, _ = self.package('--tools', str(tools)); self.assertEqual(rc, 1); self.assertRegex(err, r'missing .*nx_extract: a prebuilt tool')

    def test_tools_windows(self):   # the .exe stand-ins in the zip's bin/; the check does not run them but holds the translator to the lock
        win = self.tmp / 'build/win64'; self.fake_tree(win, '.exe')
        tools = self.tools('.exe')
        rc, err, out = self.package('--tools', str(tools), '--archive', '--check', platform='windows', build=win, aot=False)
        self.assertEqual(rc, 1); self.assertIn('does not match the lock: dmsh_xlate_vk_win-x64', err)
        with zipfile.ZipFile(out.parent / (out.name + '.zip')) as z:
            self.assertIn('nxnative-windows-x86_64/bin/nx_extract.exe', z.namelist()); self.assertIn('nxnative-windows-x86_64/bin/dmsh_xlate_vk.exe', z.namelist())
            self.assertIn(b'bin/nx_extract.exe', z.read('nxnative-windows-x86_64/README.txt'))

    @unittest.skipUnless(sys.platform == 'linux', 'the Linux bundle\'s check runs its programs')
    def test_check(self):
        said = []
        rc, err, out = self.package('--check', said=said)
        self.assertEqual((rc, err), (0, ''))
        self.assertTrue([s for s in said if s.startswith('check: bin/nxhost-run started with each of the 2 game profiles in the bundle') and '4 of 4 starts' in s], said)
        # a file that names the build tree, a link fail it
        self.write(self.build / 'licenses/notes.txt', f'built in {self.build}\n')
        rc, err, out = self.package('--check')
        self.assertEqual(rc, 1); self.assertIn(f'licenses/notes.txt names {self.build}', err)
        (self.build / 'licenses/notes.txt').unlink(); self.assertEqual(self.package()[0], 0)
        (out / 'link').symlink_to('README.txt')
        with mock.patch.object(pp, 'say', lambda m: None): fails = pp.check('linux', out, [self.build, self.aot])
        self.assertEqual(fails, ['link is a symbolic link'])
        (out / 'link').unlink()
        # a host built from a checkout that lacks one of the bundle's profiles: its --game start is refused, and the check says which
        self.fake_tree(self.build, '', table=GAMES[:1])
        rc, err, out = self.package('--check')
        self.assertEqual(rc, 1); self.assertIn('the host does not start with every game profile of the bundle (1 of 4 starts):', err)
        self.assertIn(f'bin/nxhost-run --game {GAMES[1]}: exit 2 (2 wanted: its usage), no line \'[dm] game profile {GAMES[1]} (Super Mario Odyssey): the table built into the program (--game)\'', err)
        self.assertIn(f'[RED] --game {GAMES[1]}: no such game profile', err)
        # a host that does not read the bundle's profile files (another key list): its --profile starts fail
        self.fake_tree(self.build, '')
        (self.build / 'nxhost-run').write_text(HOST.replace('@TABLE@', ' '.join(GAMES)).replace('--profile) echo', '--profile) echo "[dm] [RED] game profile file $2 (--profile): unknown key" >&2; exit 2; echo'))
        rc, err, out = self.package('--check')
        self.assertEqual(rc, 1); self.assertIn('(2 of 4 starts):', err); self.assertIn(f'bin/nxhost-run --profile {GAMES[0]}: exit 2', err)
        # a broken launcher and a chain file missing from the bundle
        self.fake_tree(self.build, ''); self.assertEqual(self.package()[0], 0)
        self.write(out / 'nxlauncher', '#!/bin/sh\nexit 1\n', 0o755); (out / 'dm/gfx/tools/dmsh_spv.py').unlink()
        with mock.patch.object(pp, 'say', lambda m: None): fails = pp.check('linux', out, [self.build, self.aot])
        self.assertTrue(any(f.startswith('nxlauncher: exit 1') for f in fails), fails)
        self.assertTrue(any(f.startswith('dm/gfx/tools/dmsh_chain_vk.py: exit 1') for f in fails), fails)   # it hashes a file the bundle lacks

    @unittest.skipUnless(sys.platform == 'linux', 'the Linux bundle\'s check runs its programs')
    def test_check_dynamic_nxaot(self):   # an nxaot linked with the shared LLVM (its DT_NEEDED names libLLVM, and libxml2 through it) fails the check; the static one passes
        rc, err, out = self.package()
        self.assertEqual((rc, err), (0, ''))
        def check(needed):
            dyn = {'bin/nxaot': (needed, [])}
            with mock.patch.object(pp, 'elf_dynamic', lambda p: dyn.get(Path(p).relative_to(out).as_posix())), mock.patch.object(pp, 'say', lambda m: None):
                return pp.check('linux', out, [self.build, self.aot])
        self.assertEqual(check(['liblzma.so.5', 'libm.so.6', 'libz.so.1', 'libzstd.so.1', 'libtinfo.so.6', 'libstdc++.so.6', 'libgcc_s.so.1', 'libc.so.6']), [])
        self.assertEqual(check(['libLLVM.so.18.1', 'libxml2.so.2', 'libstdc++.so.6', 'libc.so.6']),
                         ['bin/nxaot needs libLLVM.so.18.1 libxml2.so.2, which a player\'s PC need not have: it was linked with the shared LLVM; build nxaot with '
                          '-DNXAOT_LLVM_STATIC=ON (dm/tools/aot/README.md)'])

    def spirv(self, exe):   # stand-ins for docker/build_spirv_tools.sh's output: the four tools (Linux: each prints its row's line of the lock; Windows: a PE header), their licences
        d, lock = self.tmp / f'build/port_spirv/{"win-x64" if exe else "linux-x64"}', dmsh_chain_vk.lock()
        for row, t, opt in pp.SPIRV_ROWS:
            if exe: (d / f'{t}{exe}').parent.mkdir(parents=True, exist_ok=True); (d / f'{t}{exe}').write_bytes(PE); (d / f'{t}{exe}').chmod(0o755)
            else: self.write(d / t, f"#!/bin/sh\necho '{lock[row + '_linux-x64']}'\n", 0o755)
        for n in pp.SPIRV_LICENCES: self.write(d / 'licenses' / n, f'{n} of the SPIR-V tools\n')
        return d

    def python(self, exe, more=''):   # a stand-in for dm/tools/publish_port_python.py's output: the runtime (PYTHON on Linux; the files the check looks for on Windows), python.tsv, licences
        d = self.tmp / f'build/port_python/{"win-x64" if exe else "linux-x64"}'
        program = 'python/python.exe' if exe else 'python/bin/python3.14'
        if exe:
            for rel in ('python.exe', 'python314.dll', 'python314.zip', 'Lib/site-packages/lz4/block/_block.cp314-win_amd64.pyd', 'Lib/site-packages/capstone/lib/capstone.dll',
                        'Lib/site-packages/fontTools/ttLib/__init__.py'):
                self.write(d / 'python' / rel, f'{rel}\n', 0o755 if rel.endswith(('.exe', '.dll', '.pyd')) else 0o644)
        else:
            self.write(d / program, PYTHON.replace('@PY@', sys.executable).replace('@MORE@', more), 0o755)
            self.write(d / 'python/lib/python3.14/os.py', '# the standard library\n')
        self.write(d / 'python.tsv', f'# the packer\'s Python\nprogram\t{program}\npython\t3.14.10\nlz4\t4.4.5\n')
        for n in ('CPython.txt', 'lz4.txt', 'capstone.txt', 'fontTools.txt'): self.write(d / 'licenses' / n, f'{n} of the Python\n')
        return d

    @unittest.skipUnless(sys.platform == 'linux', 'the Linux bundle\'s check runs its programs')
    def test_python_and_spirv(self):   # --python and --spirv: python/ and bin/ in the bundle, pack.sh runs the bundle's Python, README.txt asks for nothing; the check holds them to the lock
        tools, py, spv = self.tools(''), self.python(''), self.spirv('')
        self.write(tools / 'dmsh_xlate_vk', f'#!/bin/sh\n[ "$1" = --source-sha ] && {{ echo {dmsh_chain_vk.xlate_source_sha()}; exit 0; }}; exit 2\n', 0o755)
        rc, err, out = self.package('--tools', str(tools), '--python', str(py), '--spirv', str(spv))
        self.assertEqual((rc, err), (0, ''))
        files = dict(pp.bundle_files(out))
        for t in pp.SPIRV_TOOLS: self.assertEqual(files[f'bin/{t}'], 0o755, t)
        self.assertEqual((files['python/bin/python3.14'], files['python/lib/python3.14/os.py']), (0o755, 0o644))
        self.assertNotIn('python.tsv', [Path(r).name for r in files])
        for n in pp.SPIRV_LICENCES + ('CPython.txt', 'lz4.txt', 'fontTools.txt'): self.assertIn(f'licenses/{n}', files, n)
        version = (out / 'VERSION').read_text().splitlines()
        self.assertIn('python 3.14.10 python/bin/python3.14', version); self.assertIn('spirv yes', version)
        sh = (out / 'pack.sh').read_text()
        self.assertIn('    unset PYTHONHOME PYTHONPATH; export PYTHONNOUSERSITE=1', sh); self.assertIn('    PYTHON=$here/python/bin/python3.14\n', sh)
        self.assertIn('exec "$PYTHON" "$here/scripts/build_ipa_from_nsp.py" --platform linux "$@"', sh)
        readme = flat((out / 'README.txt').read_text())
        self.assertIn('To pack a game: nothing to install. The packer runs with this folder\'s own Python 3.14.10 (python/, with the modules it needs; PYTHON=<path> names '
                      'another); its extraction tool and Vulkan shader translator and SPIR-V tools (SPIRV-Tools, glslang, SPIRV-Cross) are in bin/, and none of them needs .NET.', readme)
        for absent in ('pip install', '.NET SDK', 'does not carry'): self.assertNotIn(absent, readme)
        self.assertRegex((out / 'README.txt').read_text(), r'\n  bin/spirv-val +the SPIR-V validator .*\n  bin/spirv-link .*\n  bin/spirv-cross .*\n  bin/glslangValidator +glslang, .*\n'
                                                           r'  python/ +the packer\'s Python 3\.14\.10 with lz4, capstone and fontTools\n')
        self.assertIn('runs the Python the game was packed with (its launch file\'s python row): after a move, pack the game once more with this folder\'s ./pack.sh.', readme)
        # the check: with the stand-in translator's fingerprint in the bundle's lock (and its header written again), everything passes, dmsh_chain_vk.py --check-tools with bin/ only
        lock = out / 'dm/gfx/tools/dmsh_chain_vk.lock'
        lock.write_text(lock.read_text().replace(dmsh_chain_vk.lock()['dmsh_xlate_vk_linux-x64'], dmsh_chain.sha16(out / 'bin/dmsh_xlate_vk')))
        subprocess.run([sys.executable, '-I', out / 'dm/gfx/tools/dmsh_chain_vk.py', '--header'], check=True, capture_output=True)
        said = []
        with mock.patch.object(pp, 'say', said.append): self.assertEqual(pp.check('linux', out, [self.build, self.aot, tools, py, spv]), [])
        for line in ('check: the bundle\'s Python: every sys.path entry is inside the bundle (or the current folder)',
                     'check: dm/gfx/tools/dmsh_chain_vk.py --check-tools (python3.14): passed with PATH holding only the bundle\'s bin/ and XLATE_VK its translator'):
            self.assertIn(line, said)
        self.assertTrue([s for s in said if s.startswith('check: sh pack.sh --help: exit 0: usage: build_ipa_from_nsp.py')], said)
        self.assertTrue([s for s in said if s.startswith('check: bin/spirv-cross --revision: exit 0: Git commit: vulkan-sdk-1.4.363.0')], said)
        # a tool of another version, a Python whose sys.path leaves the bundle
        self.write(out / 'bin/spirv-val', "#!/bin/sh\necho 'SPIRV-Tools v2024.4 unknown hash'\n", 0o755)
        self.write(out / 'python/bin/python3.14', PYTHON.replace('@PY@', sys.executable).replace('@MORE@', 'echo "path /usr/lib/python3/dist-packages";'), 0o755)
        with mock.patch.object(pp, 'say', lambda m: None): fails = pp.check('linux', out, [self.build, self.aot, tools, py, spv])
        self.assertIn("bin/spirv-val --version prints 'SPIRV-Tools v2024.4 unknown hash', not the spirv-tools_linux-x64 row of the bundle's lock, "
                      f"'{dmsh_chain_vk.lock()['spirv-tools_linux-x64']}'", fails)
        self.assertIn("the bundle's Python has sys.path entries outside the bundle: ['/usr/lib/python3/dist-packages']", fails)
        self.assertTrue([f for f in fails if f.startswith(f'dm/gfx/tools/dmsh_chain_vk.py --check-tools with PATH={out / "bin"}: exit 1') and 'spirv-tools_linux-x64' in f], fails)
        # what each input must hold
        (spv / 'licenses/glslang.txt').unlink()
        rc, err, _ = self.package('--python', str(py), '--spirv', str(spv)); self.assertEqual(rc, 1); self.assertRegex(err, r'missing .*glslang.txt: a licence of the SPIR-V tools')
        (spv / 'spirv-cross').unlink()
        rc, err, _ = self.package('--spirv', str(spv)); self.assertEqual(rc, 1); self.assertRegex(err, r'missing .*spirv-cross: a SPIR-V tool \(sh docker/build_spirv_tools.sh linux')
        (py / 'licenses/CPython.txt').unlink()
        rc, err, _ = self.package('--python', str(py)); self.assertEqual(rc, 1); self.assertRegex(err, r'missing .*CPython.txt: the licence of the packer\'s Python')
        (py / 'python.tsv').unlink()
        rc, err, _ = self.package('--python', str(py)); self.assertEqual(rc, 1); self.assertRegex(err, r'missing .*python.tsv: what the packer\'s Python holds')
        (py / 'python/bin/link').symlink_to('python3.14'); self.write(py / 'python.tsv', 'program\tpython/bin/python3.14\npython\t3.14.10\n'); self.write(py / 'licenses/CPython.txt', 'x\n')
        rc, err, _ = self.package('--python', str(py)); self.assertEqual(rc, 1); self.assertRegex(err, r'python/bin/link is a symbolic link: a bundle holds none')

    def test_readme_says_what_is_missing(self):   # each input left out is named in "What this PC needs", and only those
        profiles = list(GAMES)
        no_spirv = flat(pp.readme('linux', 'abc1234', '2026-11-15', profiles, True, True, '3.14.10', False))
        self.assertIn('To pack a game: nothing to install.', no_spirv); self.assertIn('SPIRV-Cross of the versions dm/gfx/tools/dmsh_chain_vk.lock records, which this folder does not carry', no_spirv)
        no_python = flat(pp.readme('windows', 'abc1234', '2026-11-15', profiles, True, True, None, True))
        self.assertIn('To pack a game: Python 3.12 or later with lz4, capstone, fonttools and backports.zstd (py -3 -m pip install lz4 capstone fonttools backports.zstd;', no_python)
        self.assertIn("The packer's extraction tool and Vulkan shader translator and SPIR-V tools (SPIRV-Tools, glslang, SPIRV-Cross) are in bin/", no_python)
        self.assertNotIn('does not carry', no_python); self.assertNotIn('DejaVu', no_python)   # Windows has Arial
        everything = pp.readme('windows', 'abc1234', '2026-11-15', profiles, True, True, '3.14.10', True)
        self.assertIn('nothing to install', flat(everything)); self.assertNotIn('pip', everything); self.assertIn('\r\n', everything)
        self.assertNotIn('Linux program', everything)
        self.assertIn('The recompiler nxaot is a Linux program: this folder has none', flat(pp.readme('windows', 'abc1234', '2026-11-15', profiles, False, True, '3.14.10', True)))
        self.assertTrue(all(len(l) <= 172 for l in everything.splitlines()), [l for l in everything.splitlines() if len(l) > 172])

    def test_python_and_spirv_windows(self):   # the .exe files in the zip; pack.cmd runs python\python.exe; the check finds the runtime's files and PE headers, refuses a ._pth file
        win = self.tmp / 'build/win64'; self.fake_tree(win, '.exe')
        py, spv = self.python('.exe'), self.spirv('.exe')
        rc, err, out = self.package('--python', str(py), '--spirv', str(spv), '--archive', '--check', platform='windows', build=win, aot=False)
        self.assertEqual((rc, err), (0, ''))
        with zipfile.ZipFile(out.parent / (out.name + '.zip')) as z:
            names = z.namelist()
            for n in ('python/python.exe', 'python/Lib/site-packages/capstone/lib/capstone.dll', 'bin/spirv-val.exe', 'bin/glslangValidator.exe', 'licenses/SPIRV-Cross.txt'):
                self.assertIn(f'nxnative-windows-x86_64/{n}', names)
            cmd = z.read('nxnative-windows-x86_64/pack.cmd')
        self.assertIn(b'set PYTHON="%~dp0python\\python.exe"\r\n', cmd); self.assertIn(b'set "PYTHONNOUSERSITE=1"\r\n', cmd); self.assertIn(b'if defined PYTHON goto nxpack_run', cmd)
        self.assertIn(b'nxaot is a Linux program', cmd)   # no --aot
        self.assertIn('python 3.14.10 python/python.exe', (out / 'VERSION').read_text().splitlines())
        self.write(out / 'python/python314._pth', 'python314.zip\n.\n')
        (out / 'bin/spirv-link.exe').write_bytes(b'MZ not a program')
        with mock.patch.object(pp, 'say', lambda m: None): fails = pp.check('windows', out, [win, py, spv])
        self.assertEqual(fails, ['python/ holds a ._pth file: Python would run isolated, without a script\'s folder on sys.path (the packer\'s tools need it)',
                                 'bin/spirv-link.exe is not a PE32+ x86-64 program'])

    def test_spirv_versions_agree(self):   # the lines docker/build_spirv_tools.sh builds its tools to print are the lock's rows, for both platforms (one source for both)
        text = (REPO / 'docker/build_spirv_tools.sh').read_text()
        versions = dict(l.split('\t', 1) for l in re.search(r'^VERSIONS="(.*?)"$', text, re.M | re.S).group(1).splitlines())
        self.assertEqual(set(versions), {r for r, _, _ in pp.SPIRV_ROWS})
        lock = dmsh_chain_vk.lock()
        for row, line in versions.items():
            for rid in ('linux-x64', 'win-x64'): self.assertEqual(lock[f'{row}_{rid}'], line, f'{row}_{rid}')

    def test_check_windows(self):   # the executables are not run; the packer and the chain versions are
        win = self.tmp / 'build/win64'; self.fake_tree(win, '.exe')
        rc, err, out = self.package('--check', platform='windows', build=win, aot=False)
        self.assertEqual((rc, err), (0, ''))

    def test_elf_dynamic(self):   # the run path check reads an ELF's dynamic section: a real program of this system (when it is ELF)
        exe = Path(os.path.realpath(sys.executable))
        dyn = pp.elf_dynamic(exe)
        if dyn is None: self.skipTest(f'{exe} is not a 64-bit ELF file')
        self.assertTrue(any(n.startswith('libc.so') for n in dyn[0]) or dyn == ([], []), dyn)
        self.assertIsNone(pp.elf_dynamic(REPO / 'LICENSE'))


spec = importlib.util.spec_from_file_location('publish_port_python', REPO / 'dm/tools/publish_port_python.py')
ppy = importlib.util.module_from_spec(spec); spec.loader.exec_module(ppy)


class PublishPython(unittest.TestCase):   # dm/tools/publish_port_python.py without its downloads: the pins, a wheel unpacked as pip installs it, a runtime archive without links
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()).resolve(); self.addCleanup(shutil.rmtree, self.tmp)

    def test_pins(self):   # every file a platform takes is pinned by an https URL and a SHA-256; both platforms take the same modules
        for platform, p in ppy.PLATFORMS.items():
            for name in [p['runtime'], *p['wheels'], *([p['licences']] if 'licences' in p else [])]:
                url, sha = ppy.DOWNLOADS[name]
                self.assertTrue(url.startswith('https://'), url); self.assertRegex(sha, r'^[0-9a-f]{64}$')
                self.assertIn(ppy.VERSION if not name.endswith('.whl') else name.split('-')[1], url.replace('%2B', '+'))
            self.assertEqual(sorted(w.split('-')[:2] for w in p['wheels']), [['capstone', '5.0.9'], ['fonttools', '4.68.0'], ['lz4', '4.4.5']])
        self.assertTrue(all('cp314' in w or 'py3-none' in w for p in ppy.PLATFORMS.values() for w in p['wheels']))   # wheels for the runtime's CPython 3.14

    def test_wheel(self):   # the package and its .data folder's purelib / platlib into site-packages; scripts and data left out; the dist-info's licences returned
        whl = self.tmp / 'demo-1.0-py3-none-any.whl'
        with zipfile.ZipFile(whl, 'w') as z:
            z.writestr('demo/__init__.py', 'x = 1\n')
            i = zipfile.ZipInfo('demo/_native.so'); i.external_attr = 0o100755 << 16; z.writestr(i, b'\x7fELF')
            z.writestr('demo-1.0.data/purelib/demo_extra.py', 'y = 2\n')
            z.writestr('demo-1.0.data/scripts/demo-cli', '#!python\n')
            z.writestr('demo-1.0.data/data/share/man/man1/demo.1', '.TH demo\n')
            z.writestr('demo-1.0.dist-info/licenses/LICENSE', 'MIT\n'); z.writestr('demo-1.0.dist-info/METADATA', 'Name: demo\n')
        site = self.tmp / 'site'
        project, version, lic = ppy.install_wheel(whl, site)
        self.assertEqual((project, version, lic), ('demo', '1.0', [site / 'demo-1.0.dist-info/licenses/LICENSE']))
        got = sorted(p.relative_to(site).as_posix() for p in site.rglob('*') if p.is_file())
        self.assertEqual(got, ['demo-1.0.dist-info/METADATA', 'demo-1.0.dist-info/licenses/LICENSE', 'demo/__init__.py', 'demo/_native.so', 'demo_extra.py'])
        self.assertEqual(stat.S_IMODE((site / 'demo/_native.so').stat().st_mode), 0o755); self.assertEqual(stat.S_IMODE((site / 'demo/__init__.py').stat().st_mode), 0o644)
        bad = self.tmp / 'bad-1.0-py3-none-any.whl'
        with zipfile.ZipFile(bad, 'w') as z: z.writestr('../evil.py', 'x\n'); z.writestr('bad-1.0.dist-info/LICENSE', 'x\n')
        with self.assertRaisesRegex(ppy.PublishError, 'leaves its folder'): ppy.install_wheel(bad, site)

    def test_linux_runtime(self):   # python-build-standalone's tree: the interpreter and the standard library kept; links, tkinter, pip and the other left-out parts not
        arc = self.tmp / 'pbs.tar.gz'
        with tarfile.open(arc, 'w:gz') as t:
            def add(name, data=b'x', mode=0o644, link=None):
                ti = tarfile.TarInfo(name); ti.mode = mode
                if link: ti.type, ti.linkname = tarfile.SYMTYPE, link; t.addfile(ti)
                else: ti.size = len(data); t.addfile(ti, io.BytesIO(data))
            add('python/bin/python3.14', mode=0o755); add('python/bin/python3', link='python3.14'); add('python/bin/pip3', mode=0o755)
            add('python/lib/python3.14/os.py'); add('python/lib/python3.14/tkinter/__init__.py'); add('python/lib/python3.14/site-packages/pip/__init__.py')
            add('python/lib/python3.14/site-packages/README.txt'); add('python/lib/libpython3.14.so.1.0'); add('python/share/terminfo/x/xterm')
        out = self.tmp / 'out'
        with mock.patch.object(ppy, 'say', lambda m: None): ppy.linux_runtime(arc, out)
        got = sorted(p.relative_to(out).as_posix() for p in out.rglob('*') if p.is_file() or p.is_symlink())
        self.assertEqual(got, ['python/bin/python3.14', 'python/lib/python3.14/os.py', 'python/lib/python3.14/site-packages/README.txt'])
        self.assertEqual(stat.S_IMODE((out / 'python/bin/python3.14').stat().st_mode), 0o755)


if __name__ == '__main__':
    unittest.main()
