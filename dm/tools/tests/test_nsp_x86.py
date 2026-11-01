"""NSP script packs for Windows and Linux (scripts/build_ipa_from_nsp.py --platform linux / windows): the recompiler's input (patch_nso.py --aot, one run beside Apple's outputs) and nro.tsv in
<id>_build/aot_<version>/, nxaot build of every module and NRO into <name>.aot.so, the Vulkan shader steps (dmsh_build_vk.py, gl_shaders.py --vk, none for a guest Vulkan game), the run script,
Windows stopping at the packs, and the reuse of every step on a second run. Real fingerprints, a fake nxaot that writes its pack, mock runs of the other tools; synthetic NSO headers, a fake NRO
in a fake RomFS and a synthetic control.nacp: no game files, keys, .NET, Docker or Xcode needed (temporary files only)."""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games')); sys.path.insert(0, str(REPO / 'dm/tools'))
import game, nacp, profile_guess
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
nsp.pg, nsp.nacp = profile_guess, nacp   # main sets them

TID = 0x0100AAAA00000000
ID = f'{TID:016X}'
BID, BID2 = 'ab' * 16, 'cd' * 16
REAL_POPEN, REAL_RUN, TRUE = subprocess.Popen, nsp.run, shutil.which('true')   # TRUE: what every tool the script looks for on PATH is found as
NXAOT ='''#!{py}
# a stand-in for nxaot: `nxaot build` with no input prints the usage (exit 2); a build writes the object and the pack
import os, sys
a = sys.argv[1:]
usage = 'usage: nxaot build <module.nro | name.aot.tsv> -o <object.o> [--shared <lib.so>] [--mode balanced|max]' + ('' if os.environ.get('FAKE_NXAOT_OLD') else ' [--workers N] [--cache <dir>]')
if a[:1] != ['build'] or len(a) < 2: sys.stderr.write(usage + '\\n'); sys.exit(2)
src, obj, so = a[1], a[a.index('-o') + 1], a[a.index('--shared') + 1]
with open(src, 'rb') as f: head = f.read(64)
with open(obj, 'wb') as f: f.write(b'object')
with open(so, 'wb') as f: f.write(b'\\x7fELF pack of ' + head)
print('aot:', os.path.basename(src), 'packed')
'''


def nso(path, bid):   # an NSO header: the build ID at +0x40
    path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(b'NSO0' + bytes(0x3c) + bytes.fromhex(bid) + bytes(0x50))


def control(path, name, disp):   # control.nacp: the American English title at 0, the supported languages at 0x302C (en-US), the display version at 0x3060
    b = bytearray(0x4000); b[:len(name)] = name.encode(); b[0x302C:0x3030] = (1).to_bytes(4, 'little'); b[0x3060:0x3060 + len(disp)] = disp.encode()
    path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(bytes(b))


def sources(gfx):   # profile_guess.build_sources: the graphics API and no run-time shaders
    return {'graphics': (gfx, f'a {gfx} game'), **{k: (False, '') for k in ('glslc_capture', 'vk_capture', 'gl_capture', 'ue_decode', 'oodle_packs', 'ue_legacy')}}


def loader_rows(text):
    """nro.tsv as dm/host/loader.c reads it (aot_read_nros, preload_nros): every line but '#' and empty ones is sscanf("%15s %64s %u"): a name of at most 15 characters, 64 hex digits, a size"""
    rows = []
    for line in text.splitlines(True):
        if line[0] in '#\n': continue
        name, sha, size = line.split()[:3]
        assert len(name) <= 15 and len(sha) == 64 and int(sha, 16) >= 0 and int(size) >= 0, line
        rows.append((name, sha, int(size)))
    return rows


class Pack(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()).resolve(); self.addCleanup(shutil.rmtree, self.tmp)
        self.prof = self.tmp / 'games' / ID; self.prof.mkdir(parents=True)
        (self.prof / 'game.tsv').write_text('\n'.join([f'title_id\t{ID}', f'program_id\t{ID}', 'name\tTest Game', 'ipa\tTestGame.ipa', 'modules\tmain', 'romfs_marker\ta.bin',
                                                       f'build\tv0\t1.0.0\tmain={BID}', 'package\tv0', 'shader_versions\tv0']) + '\n')
        for n in ('shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv'): (self.prof / n).write_text('# registered\n')
        self.nsp_dir = self.tmp / 'nsp'; self.nsp_dir.mkdir(); (self.nsp_dir / 'base.nsp').write_bytes(b'PFS0' + bytes(60))
        self.keys = self.tmp / 'prod.keys'; self.keys.write_text('# none\n')
        self.work = self.nsp_dir / f'{ID}_build'; self.wdir = self.nsp_dir / f'{ID}-work'
        self.game = self.work / 'extract/v0'   # extracted already: extract() is mocked, and the free-space check passes for a version that is there
        nso(self.game / 'ExeFS/main', BID); control(self.game / 'control.nacp', 'Test Game', '1.0.0')
        (self.game / 'RomFS').mkdir(); (self.game / 'RomFS/a.bin').write_bytes(b'romfs'); (self.game / 'RomFS/.nrr').mkdir()
        (self.game / 'RomFS/.nrr/game.nrr').write_bytes(b'NRR0 registered')
        self.nro = b'\0' * 0x10 + b'NRO0' + bytes(0x6c)   # the module the game loads at run time, collected by nro_collect.py (mocked)
        self.nxaot = self.tmp / 'bin/nxaot'; self.nxaot.parent.mkdir(); self.nxaot.write_text(NXAOT.format(py=sys.executable)); self.nxaot.chmod(0o755)
        self.dll = self.tmp / 'xlate_vk/dmsh_xlate_vk.dll'; self.dll.parent.mkdir(); self.dll.write_bytes(b'translator')
        self.said, self.ran, self.rewrites = [], [], []
        self.gfx = 'nvn'
        fam = {'base': self.nsp_dir / 'base.nsp', 'updates': [], 'dlc': []}
        for cm in (mock.patch.dict(os.environ, {'DM_GAMES_DIR': str(self.tmp / 'games'), 'XLATE_VK': str(self.dll)}), mock.patch.dict(sys.modules, {'capstone': types.SimpleNamespace(__version__='test')}),
                   mock.patch.object(nsp, 'PROG', mock.Mock()), mock.patch.object(nsp, 'say', self.said.append),
                   mock.patch.object(nsp, 'find_repo', return_value=REPO), mock.patch.object(nsp, '_importable', return_value=True),
                   mock.patch.object(nsp.shutil, 'which', side_effect=lambda name, **kw: TRUE),
                   mock.patch.object(nsp, 'scan_nsps', return_value=(TID, fam)), mock.patch.object(nsp, 'decompress_nsz', return_value=fam),
                   mock.patch.object(nsp, 'build_nx_extract', return_value='nx_extract'), mock.patch.object(nsp, 'extract', return_value=self.game),
                   mock.patch.object(nsp, 'split_shaders', side_effect=self.split), mock.patch.object(profile_guess, 'build_sources', side_effect=lambda *a: sources(self.gfx)),
                   mock.patch.object(nsp, 'run', side_effect=self.fake_run), mock.patch.object(nsp.subprocess, 'Popen', side_effect=self.popen)):
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    # ---- the tools ----
    def popen(self, cmd, **kw):   # patch_nso.py (rewrite's processes): Apple's outputs, and with --aot the recompiler's input; anything else runs (nxaot through the real run())
        if len(cmd) > 1 and Path(cmd[1]).name == 'patch_nso.py':
            self.rewrites.append(cmd[2:])
            src, out, name = Path(cmd[2]), Path(cmd[3]), cmd[4]
            (out / f'{name}.patched.bin').write_bytes(b'rewritten ' + src.read_bytes()[:0x50])
            (out / f'{name}.fixups.tsv').write_text('# PATCH_OFF\t0x100\n')
            if '--aot' in cmd:
                aot = Path(cmd[cmd.index('--aot') + 1]); image = b'image of ' + src.read_bytes()[:0x50]
                (aot / f'{name}.image.bin').write_bytes(image)
                (aot / f'{name}.aot.tsv').write_text(f'# nxaot-manifest 1\t{name}\t{BID}\t{hashlib.sha256(image).hexdigest()}\n')
            return types.SimpleNamespace(poll=lambda: 0, returncode=0, terminate=lambda: None, wait=lambda *a: 0)
        return REAL_POPEN(cmd, **kw)

    def fake_run(self, cmd, log, env=None, cwd=None, on_line=None, ok=(0,)):
        cmd = [str(c) for c in cmd]; log = Path(log); log.parent.mkdir(parents=True, exist_ok=True)
        tool = Path(cmd[1] if cmd[0] == sys.executable else cmd[0]).name
        self.ran.append((tool, cmd))
        if tool == 'nxaot': return REAL_RUN(cmd, log, env=env, cwd=cwd, on_line=on_line, ok=ok)   # the fake nxaot, run for real
        if tool == 'nro_collect.py':   # the registered NRO, found loose: nro.tsv as nro_collect.py writes it (a refused online SDK too)
            out = Path(cmd[3]); out.mkdir(parents=True); (out / 'nro0.nro').write_bytes(self.nro)
            (out / 'nro.tsv').write_text('# name\tsha256\tsize\tnrr\twhere\n' + f'nro0\t{hashlib.sha256(self.nro).hexdigest()}\t{len(self.nro)}\tgame.nrr\tloose a.nro\n'
                                         + f'refused\t{"ef" * 32}\t256\tgame.nrr\tloose eos.nro (EOS_Platform_Create)\n')
        elif tool in ('dmsh_chain.py', 'dmsh_chain_vk.py'): pass   # --header: the repository's headers are not written by a test
        elif tool == 'dmsh_build_vk.py':   # a full build: the library, its stamp and the profile's tables naming it
            lib = Path(cmd[3]) / f'vk-{nsp.chain_vk_of(REPO)}'; lib.mkdir(parents=True, exist_ok=True); (lib / '0123456789abcdef.spv').write_bytes(b'spv')
            (lib / 'inputs.tsv').write_text(f'# stamp\ngame\t{ID}\ninputs\tdigest\n')
            for n in nsp.CHAIN_TABLES_VK:   # into the profile DM_GAME names now (a derived one comes first)
                (game.load(ID).dir / n).write_text(f'# chain {lib.name} inputs digest 2026-11-01T00:00:00+00:00\n')
            log.write_text('shaders 1: ok 1\nshader cache: hit 0, built 1\n')
        elif tool == 'gl_shaders.py':
            assert cmd[-1] == '--vk', cmd
            out = Path(cmd[-2]) / 'gl-vk'; out.mkdir(parents=True, exist_ok=True); (out / 'gl_shaders.tsv').write_text('# GL shaders for Vulkan\n')
            log.write_text(f'vk: 2 shaders (1 vertex) → {out}\n')
        else: raise AssertionError(f'unexpected tool run: {cmd}')
        return 0

    def split(self, py, repo, roms, mains, pkg, env, logs, g, *a):   # the shader split (test_nsp_incremental.py tests it): the package and its key
        pkg.mkdir(parents=True, exist_ok=True); (pkg / 'main.bin').write_bytes(b'package'); return nsp.package_key(pkg)

    # ---- helpers ----
    def pack(self, platform='linux', *extra):
        self.ran.clear(); self.rewrites.clear(); self.said.clear()
        args = nsp.parse_args([str(self.nsp_dir), '-k', str(self.keys), '--platform', platform] + (['--nxaot', str(self.nxaot)] if platform == 'linux' else []) + list(extra))
        nsp.main(args)

    def tools(self, name): return [c for t, c in self.ran if t == name]

    @property
    def aot(self): return self.work / 'aot_v0'


class Linux(Pack):
    def test_layout_invocations_and_reuse(self):
        self.pack()
        aot, nro_dir = self.aot, self.work / 'nro_v0'
        # the rewrite: one run per module, the profile's modules with --aot into the folder's partial copy, the NRO without (the host maps it from the game's image)
        self.assertEqual(sorted(self.rewrites), sorted([[str(self.game / 'ExeFS/main'), str(self.work / 'patched_v0.partial'), 'main', '--aot', str(self.work / 'aot_v0.partial')],
                                                        [str(nro_dir / 'nro0.nro'), str(self.work / 'patched_v0.partial'), 'nro0']]))
        self.assertTrue((self.work / 'patched_v0/main.patched.bin').is_file())   # Apple's outputs from the same run
        # the folder: manifests and images, nro.tsv as the loader reads it, a pack per module and per NRO, nothing for a refused SDK
        self.assertEqual(sorted(p.name for p in aot.iterdir()), ['.keys', 'main.aot.so', 'main.aot.tsv', 'main.image.bin', 'nro.tsv', 'nro0.aot.so'])
        self.assertEqual(loader_rows((aot / 'nro.tsv').read_text()), [('nro0', hashlib.sha256(self.nro).hexdigest(), len(self.nro)), ('refused', 'ef' * 32, 256)])
        self.assertEqual((aot / 'nro.tsv').read_bytes(), (self.work / 'patched_v0/nro.tsv').read_bytes())   # Apple's rows, unchanged
        self.assertTrue((aot / 'main.aot.so').read_bytes().startswith(b'\x7fELF pack of # nxaot-manifest 1'))
        self.assertEqual((aot / 'nro0.aot.so').read_bytes(), b'\x7fELF pack of ' + self.nro[:64])   # packed from the NRO file itself
        # nxaot: the manifest and the NRO file, the object in the temporary folder, the pack beside the input, the mode, the cache in the work folder (this nxaot has --cache)
        tmp = self.work / 'tmp/aot'
        self.assertEqual(self.tools('nxaot'), [
            [str(self.nxaot), 'build', str(aot / 'main.aot.tsv'), '-o', str(tmp / 'main.o'), '--shared', str(tmp / 'main.aot.so'), '--mode', 'balanced', '--cache', str(self.wdir / 'aot-cache')],
            [str(self.nxaot), 'build', str(nro_dir / 'nro0.nro'), '-o', str(tmp / 'nro0.o'), '--shared', str(tmp / 'nro0.aot.so'), '--mode', 'balanced', '--cache', str(self.wdir / 'aot-cache')]])
        self.assertTrue((self.wdir / 'aot-cache').is_dir())
        # the NVN shaders: dmsh_build_vk.py from the split's package into the game's shader folder
        self.assertEqual(self.tools('dmsh_build_vk.py'), [[sys.executable, str(REPO / 'dm/gfx/tools/dmsh_build_vk.py'), str(self.work / 'shader/pkg'), str(self.work / 'shader/cache')]])
        self.assertEqual(self.tools('gl_shaders.py'), [])
        # the run script
        sh = (self.work / 'run.sh').read_text()
        self.assertTrue(os.access(self.work / 'run.sh', os.X_OK))
        self.assertIn(f'exec "${{NXHOST:-{REPO / "build/linux/nxhost-run"}}}" {self.aot} "$@"', sh)
        for k, v in (('DM_ROMFS', self.game / 'RomFS'), ('DM_SAVE', self.wdir / 'run/save'), ('DM_TEMP', self.wdir / 'run/temp'), ('DM_MSL_DIR', self.work / 'shader/cache'),
                     ('DM_NVN_SHMAP', self.prof / 'shader_map.tsv'), ('DM_LANG', 'en-US'), ('DM_WINDOW', 1), ('DM_AUDIO', 1)):
            self.assertIn(f'\nexport {k}={v}\n', sh)
        self.assertIn(f'-DGAME={ID}', sh); self.assertNotIn('DM_GAMES_DIR', sh)   # a registered profile
        self.assertTrue((self.wdir / 'run/save').is_dir())
        self.assertFalse((self.work / 'tmp').exists())
        # a second run reuses every step: no rewrite, no pack, no shader build
        first = {p.name: p.read_bytes() for p in aot.iterdir() if p.is_file()}
        self.pack()
        self.assertEqual((self.rewrites, self.tools('nxaot'), self.tools('dmsh_build_vk.py')), ([], [], []))
        self.assertEqual({p.name: p.read_bytes() for p in aot.iterdir() if p.is_file()}, first)
        # a new NRO: the rewrite runs again, and only the NRO's pack is made again (main's manifest is the same)
        self.nro = self.nro[:-4] + b'new!'; (self.game / 'RomFS/.nrr/game.nrr').write_bytes(b'NRR0 registered again')
        self.pack()
        self.assertEqual(len(self.rewrites), 2)
        self.assertEqual([Path(c[2]).name for c in self.tools('nxaot')], ['nro0.nro'])
        self.assertEqual(loader_rows((aot / 'nro.tsv').read_text())[0][1], hashlib.sha256(self.nro).hexdigest())
        # another pack mode packs everything again
        self.pack('linux', '--pack-mode', 'max')
        self.assertEqual([c[c.index('--mode') + 1] for c in self.tools('nxaot')], ['max', 'max'])

    def test_removed_module_and_nros_leave_no_stale_input(self):
        self.pack()
        (self.game / 'RomFS/.nrr/game.nrr').unlink()   # an update without NRRs
        self.pack()
        self.assertEqual(sorted(p.name for p in self.aot.iterdir()), ['.keys', 'main.aot.so', 'main.aot.tsv', 'main.image.bin'])

    def test_old_nxaot_without_cache_and_workers(self):
        with mock.patch.dict(os.environ, {'FAKE_NXAOT_OLD': '1'}):
            self.assertEqual(nsp.nxaot_options(self.nxaot, dict(os.environ)), set())
            self.pack('linux', '--aot-workers', '3')
        for c in self.tools('nxaot'): self.assertNotIn('--cache', c); self.assertNotIn('--workers', c)
        self.assertIn('this nxaot has no --cache option: every function is compiled again on each pack', self.said)
        self.assertEqual(nsp.nxaot_options(self.nxaot, dict(os.environ)), {'--cache', '--workers'})
        (self.aot / 'main.aot.so').unlink(); self.pack('linux', '--aot-workers', '3')
        self.assertEqual(self.tools('nxaot')[0][-4:], ['--cache', str(self.wdir / 'aot-cache'), '--workers', '3'])

    def test_nxaot_missing_or_broken(self):
        repo = self.tmp / 'repo'
        with self.assertRaisesRegex(nsp.BuildError, r'nxaot is not built .*docker build -t nxnative-aot .*--target nxaot'): nsp.find_nxaot(repo)
        (repo / 'build/aot').mkdir(parents=True); (repo / 'build/aot/nxaot').write_text('')
        self.assertEqual(nsp.find_nxaot(repo), repo / 'build/aot/nxaot')
        with self.assertRaisesRegex(nsp.BuildError, 'no such file'): nsp.find_nxaot(repo, str(self.tmp / 'none'))
        broken = self.tmp / 'bin/broken'; broken.write_text('#!/bin/sh\necho "error while loading shared libraries: libLLVM-18.so" >&2; exit 127\n'); broken.chmod(0o755)
        with self.assertRaisesRegex(nsp.BuildError, 'libLLVM-18.so.*NXAOT_LLVM_STATIC'): nsp.nxaot_options(broken, dict(os.environ))
        with self.assertRaisesRegex(nsp.BuildError, 'does not run'): nsp.nxaot_options(self.tmp / 'none', dict(os.environ))
        # main stops before any long step
        args = nsp.parse_args([str(self.nsp_dir), '-k', str(self.keys), '--platform', 'linux', '--nxaot', str(broken)])
        with self.assertRaisesRegex(nsp.BuildError, 'does not run as nxaot'): nsp.main(args)
        self.assertEqual((self.ran, self.rewrites), ([], []))

    def test_without_the_vulkan_translator(self):   # the NVN shaders are reported as not run; the rest is packed
        with mock.patch.dict(os.environ, {'XLATE_VK': str(self.tmp / 'none.dll')}): self.pack()
        self.assertEqual(self.tools('dmsh_build_vk.py'), [])
        self.assertTrue(any(m.startswith('[RED] the NVN shaders were not translated for Vulkan (no Vulkan translator at') for m in self.said))
        self.assertIn('[RED] not run on this system: the NVN shaders for Vulkan (no Vulkan translator at ' + str(self.tmp / 'none.dll') + ')', self.said)
        self.assertTrue((self.aot / 'main.aot.so').is_file())
        self.pack()   # with it: the step runs
        self.assertEqual(len(self.tools('dmsh_build_vk.py')), 1)

    def test_stale_vulkan_tables_build_again(self):
        self.pack()
        (self.prof / 'cb_extent_vk.tsv').write_text('# chain vk-000000000000 inputs other\n')
        self.pack(); self.assertEqual(len(self.tools('dmsh_build_vk.py')), 1)
        self.pack(); self.assertEqual(self.tools('dmsh_build_vk.py'), [])

    def test_gl_game(self):
        self.gfx = 'gl'; self.pack()
        self.assertEqual(self.tools('gl_shaders.py'), [[sys.executable, str(REPO / 'dm/gfx/tools/gl_shaders.py'), str(self.game / 'RomFS'), str(self.game / 'ExeFS/main'), str(self.work / 'shader'), '--vk']])
        self.assertEqual(self.tools('dmsh_build_vk.py'), [])
        sh = (self.work / 'run.sh').read_text()
        self.assertIn(f'\nexport DM_MSL_DIR={self.work / "shader"}\n', sh); self.assertNotIn('DM_NVN_SHMAP', sh)
        self.pack(); self.assertEqual(self.tools('gl_shaders.py'), [])

    def test_vulkan_game(self):
        self.gfx = 'vulkan'; self.pack()
        self.assertEqual((self.tools('dmsh_build_vk.py'), self.tools('gl_shaders.py')), ([], []))
        self.assertFalse((self.work / 'shader').exists())
        self.assertTrue(any('no shader step on this platform' in m for m in self.said))
        self.assertTrue((self.aot / 'main.aot.so').is_file())

    def test_derived_profile(self):   # files of another build under the registered tag: packed from a derived profile, which nxhost-run must be configured with
        nso(self.game / 'ExeFS/main', BID2); self.pack()
        pdir = self.work / 'profile'
        self.assertEqual(game.Game(pdir / ID).builds()['v0']['main'], BID2)
        sh = (self.work / 'run.sh').read_text()
        self.assertIn(f'with DM_GAMES_DIR={pdir} and DM_GAMES_FIRST=1', sh)
        self.assertIn(f'\nexport DM_NVN_SHMAP={pdir / ID / "shader_map.tsv"}\n', sh)
        self.pack()
        self.assertEqual((self.rewrites, self.tools('nxaot')), ([], []))

    def test_options_belong_to_the_port(self):
        with self.assertRaisesRegex(nsp.BuildError, '--platform linux / windows'):
            nsp.main(nsp.parse_args([str(self.nsp_dir), '-k', str(self.keys), '--platform', 'ios', '--pack-mode', 'max']))
        with self.assertRaisesRegex(nsp.BuildError, '-o names the IPA'):
            nsp.main(nsp.parse_args([str(self.nsp_dir), '-k', str(self.keys), '--platform', 'linux', '-o', str(self.tmp / 'x.ipa')]))


class Windows(Pack):
    def test_stops_at_the_packs_after_everything_else(self):
        with self.assertRaisesRegex(nsp.BuildError, r'the packs are not built: .*phase 5b'): self.pack('windows')
        self.assertEqual(self.tools('nxaot'), [])
        self.assertEqual(sorted(p.name for p in self.aot.iterdir()), ['main.aot.tsv', 'main.image.bin', 'nro.tsv'])
        self.assertEqual(len(self.tools('dmsh_build_vk.py')), 1)
        cmd = (self.work / 'run.cmd').read_bytes().decode()
        self.assertTrue(cmd.startswith('@echo off\r\n')); self.assertNotIn('\n', cmd.replace('\r\n', ''))
        self.assertIn(f'\r\nif not defined NXHOST set "NXHOST={REPO / "build/win64/nxhost-run.exe"}"\r\n"%NXHOST%" "{self.aot}" %*\r\n', cmd)
        self.assertIn(f'\r\nset "DM_SAVE={self.wdir / "run/save"}"\r\n', cmd)
        self.assertIn('x86_64-w64-mingw32.cmake', cmd)
        with self.assertRaisesRegex(nsp.BuildError, 'phase 5b'): self.pack('windows')   # the same stop, every step before it reused
        self.assertEqual((self.rewrites, self.tools('dmsh_build_vk.py')), ([], []))


class Apple(unittest.TestCase):   # what the Apple builds run is as it was: no --aot, an Apple rewrite's key never taken for the port's (nor the other way round)
    def test_rewrite_without_aot(self):
        tmp = Path(tempfile.mkdtemp()).resolve(); self.addCleanup(shutil.rmtree, tmp)
        exe = tmp / 'game/ExeFS'; nso(exe / 'main', BID); (exe.parent / 'RomFS').mkdir()
        g, cmds = types.SimpleNamespace(words=lambda k: ['main']), []
        def popen(cmd, **kw):
            cmds.append(cmd); (Path(cmd[3]) / f'{cmd[4]}.patched.bin').write_bytes(b'x')
            if '--aot' in cmd: d = Path(cmd[cmd.index('--aot') + 1]); (d / 'main.aot.tsv').write_text('m'); (d / 'main.image.bin').write_bytes(b'i')
            return types.SimpleNamespace(poll=lambda: 0, returncode=0)
        with mock.patch.dict(sys.modules, {'capstone': types.SimpleNamespace(__version__='test')}), mock.patch.object(nsp, 'PROG', mock.Mock()), \
             mock.patch.object(nsp.subprocess, 'Popen', side_effect=popen):
            out = tmp / 'patched_v0'
            nsp.rewrite(sys.executable, REPO, exe, out, {}, tmp / 'logs', g)
            self.assertEqual(cmds, [[sys.executable, str(REPO / 'dm/tools/patch_nso.py'), str(exe / 'main'), str(tmp / 'patched_v0.partial'), 'main']])
            nsp.rewrite(sys.executable, REPO, exe, out, {}, tmp / 'logs', g); self.assertEqual(len(cmds), 1)   # reused
            nsp.rewrite(sys.executable, REPO, exe, out, {}, tmp / 'logs', g, aot=tmp / 'aot_v0'); self.assertEqual(len(cmds), 2)   # the Apple key is not the port's
            self.assertEqual(cmds[-1][-2:], ['--aot', str(tmp / 'aot_v0.partial')])
            self.assertEqual(sorted(p.name for p in (tmp / 'aot_v0').iterdir()), ['main.aot.tsv', 'main.image.bin'])
            nsp.rewrite(sys.executable, REPO, exe, out, {}, tmp / 'logs', g, aot=tmp / 'aot_v0'); self.assertEqual(len(cmds), 2)
            nsp.rewrite(sys.executable, REPO, exe, out, {}, tmp / 'logs', g); self.assertEqual(len(cmds), 3)
            self.assertNotIn('--aot', cmds[-1])

    def test_survey_notes_only_for_the_port(self):
        S = sources('nvn') | {'glslc_capture': (True, 'GLSLC')}
        self.assertEqual(nsp.port_notes('linux', sources('vulkan'))[1], 'on linux the game\'s SPIR-V goes to the driver (dm/gfx/hostvk/guest_vk.c): no shader step')
        notes = nsp.port_notes('windows', S, ro=True)
        self.assertIn('phase 5b', notes[0]); self.assertIn('nro.tsv', notes[1]); self.assertIn('dmsh_build_vk.py', notes[2])
        self.assertTrue(notes[3].startswith('the GLSLC capture boots the game in the Mac host, which windows does not have'))


if __name__ == '__main__':
    unittest.main()
