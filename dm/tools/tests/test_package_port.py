"""The port's bundle (scripts/package_port.py; docs/development.md, "The port's bundles"): assembled from a fake build tree (stand-ins for nxlauncher, nxhost-run and nxaot,
SDL3's and LLVM's licences where the build trees keep them) and a few of this checkout's files, the missing-file errors, the archives' determinism (two runs give the same bytes,
whatever the inputs' times), and the bundle's own check where it can run (Linux: the stand-ins are run as the real programs would be). No Docker, game files or keys."""
import importlib.util
import io
import os
from pathlib import Path
import shutil
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
# the two profiles' game.tsv; the bundle's check runs on these as on the whole set
CHAIN = sorted({os.path.normpath(f'dm/gfx/{f}') for f in dmsh_chain.FILES + dmsh_chain_vk.FILES})
TREES = ('scripts/build_ipa_from_nsp.py', 'dm/tools/romfs.py', 'dm/gfx/nvn/dmsh_chain.h', 'dm/gfx/nvn/dmsh_chain_vk.h', *CHAIN, *(f'dm/games/{g}/game.tsv' for g in GAMES))
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

    def fake_tree(self, build, exe):
        """A host tree as the CI leaves it: the tree's own nxhost-run for its GAME (CMakeCache.txt), another profile's in hosts/<id>/, the launcher, SDL3's licence"""
        self.write(build / 'CMakeCache.txt', f'CMAKE_BUILD_TYPE:STRING=\nGAME:STRING={GAMES[0]}\n')
        self.write(build / f'nxhost-run{exe}', '', 0o755)
        self.write(build / f'hosts/{GAMES[1]}/nxhost-run{exe}', '', 0o755)
        self.write(build / f'nxlauncher{exe}', LAUNCHER if not exe else 'MZ launcher', 0o755)
        self.write(build / '_deps/sdl3-src/LICENSE.txt', 'SDL3 licence\n')

    def package(self, *args, platform='linux', out=None, build=None, aot=True):
        out = out or self.tmp / 'out' / f'nxnative-{platform}-x86_64'
        argv = ['--platform', platform, '--build', str(build or self.build), '--out', str(out)] + (['--aot', str(self.aot)] if aot else []) + list(args)
        err = io.StringIO()
        with mock.patch.object(sys, 'stderr', err), mock.patch.object(pp, 'say', lambda m: None):
            rc = pp.main(argv)
        return rc, err.getvalue(), out

    def test_assembly(self):
        rc, err, out = self.package()
        self.assertEqual((rc, err), (0, ''))
        files = dict(pp.bundle_files(out))
        for rel in ('nxlauncher', f'hosts/{GAMES[0]}/nxhost-run', f'hosts/{GAMES[1]}/nxhost-run', 'bin/nxaot', 'pack.sh'):
            self.assertEqual(files.pop(rel), 0o755, rel)
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
        self.assertIn(f'hosts {GAMES[1]} {GAMES[0]}', version); self.assertIn('platform linux-x86_64', version)
        readme = (out / 'README.txt').read_text()
        self.assertIn('./pack.sh "<the game\'s NSP folder>" -k "<your prod.keys>"', readme); self.assertIn('pip install lz4 capstone backports.zstd', readme)
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
        (self.build / f'hosts/{GAMES[1]}/nxhost-run').unlink()
        fails(rf'no nxhost-run in .*hosts/{GAMES[1]}')
        self.write(self.build / 'hosts/0100FFFFFFFF0000/nxhost-run', '', 0o755); shutil.rmtree(self.build / f'hosts/{GAMES[1]}')
        fails(r'built for 0100FFFFFFFF0000, which is not a game profile')
        shutil.rmtree(self.build / 'hosts'); (self.build / 'CMakeCache.txt').unlink()
        fails(r'has no GAME in')
        (self.build / 'nxhost-run').unlink()
        fails(r'no host in')
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
            self.assertEqual(modes['nxnative-linux-x86_64/README.txt'], 0o644); self.assertEqual(modes['nxnative-linux-x86_64/hosts'], 0o755)
            self.assertFalse([m for m in members if not (m.isfile() or m.isdir())])   # no link
            self.assertEqual({n[len('nxnative-linux-x86_64/'):] for n in names if tar.getmember(n).isfile()}, {r for r, _ in pp.bundle_files(out)})
        # Windows: the .exe stand-ins, no nxaot, a zip
        win = self.tmp / 'build/win64'; self.fake_tree(win, '.exe')
        self.build = win
        zp, out = both('windows', aot=False)
        with zipfile.ZipFile(zp) as z:
            infos = z.infolist(); names = [i.filename for i in infos]
            self.assertEqual(names, sorted(names))
            self.assertIn('nxnative-windows-x86_64/nxlauncher.exe', names); self.assertIn(f'nxnative-windows-x86_64/hosts/{GAMES[0]}/nxhost-run.exe', names)
            self.assertIn('nxnative-windows-x86_64/pack.cmd', names); self.assertNotIn('nxnative-windows-x86_64/bin/nxaot.exe', names)
            self.assertEqual(len({i.date_time for i in infos}), 1)
            self.assertIn(b'\r\n', z.read('nxnative-windows-x86_64/pack.cmd')); self.assertIn(b'nxaot no', z.read('nxnative-windows-x86_64/VERSION'))

    @unittest.skipUnless(sys.platform == 'linux', 'the Linux bundle\'s check runs its programs')
    def test_check(self):
        rc, err, out = self.package('--check')
        self.assertEqual((rc, err), (0, ''))
        # a file that names the build tree, a link and a host without a profile fail it
        self.write(self.build / 'licenses/notes.txt', f'built in {self.build}\n')
        rc, err, out = self.package('--check')
        self.assertEqual(rc, 1); self.assertIn(f'licenses/notes.txt names {self.build}', err)
        (self.build / 'licenses/notes.txt').unlink(); self.assertEqual(self.package()[0], 0)
        (out / 'link').symlink_to('README.txt'); shutil.rmtree(out / 'dm/games' / GAMES[1])
        with mock.patch.object(pp, 'say', lambda m: None): fails = pp.check('linux', out, [self.build, self.aot])
        self.assertIn('link is a symbolic link', fails); self.assertIn(f'hosts without a profile in dm/games: {GAMES[1]}', fails)
        # a broken launcher and a chain file missing from the bundle
        (out / 'link').unlink(); self.assertEqual(self.package()[0], 0)
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


if __name__ == '__main__':
    unittest.main()
