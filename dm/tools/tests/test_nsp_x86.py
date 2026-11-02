"""NSP script packs for Windows and Linux (scripts/build_ipa_from_nsp.py --platform linux / windows): the recompiler's input (patch_nso.py --aot, one run beside Apple's outputs) and nro.tsv in
<id>_build/aot_<version>/, nxaot build of every module and NRO into <name>.aot.so (Linux) or with --target windows into <name>.aot.dll (Windows), both pack modes side by side (max in
aot_<version>/max/), the Vulkan shader steps (dmsh_build_vk.py, gl_shaders.py --vk, none for a guest Vulkan game), the launcher's settings (launcher-<platform>.json: settings.py's call; test_settings_platforms.py tests the lists),
the launch file (launch-<platform>.tsv) and the run script that reads it: run.sh is run with a stand-in for nxhost-run and checked against the launch file's rules (the environment, then the
player's env.txt, then the env rows), an nxaot without --target stopping a Windows pack at the start, and the reuse of every step on a second run. Real fingerprints, a fake nxaot that
writes its pack, mock runs of the other tools; synthetic NSO headers, a fake NRO in a fake RomFS and a synthetic control.nacp: no game files, keys, .NET, Docker or Xcode needed
(temporary files only)."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
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
# a stand-in for nxaot: `nxaot build` with no input prints the usage (exit 2); a build writes the object and the pack (a DLL with --target windows)
import os, sys
a = sys.argv[1:]
usage = ('usage: nxaot build <module.nro | name.aot.tsv> -o <object.o> [--shared <lib.so>] [--mode balanced|max]'
         + ('' if os.environ.get('FAKE_NXAOT_OLD') else ' [--workers N] [--cache <dir>] [--target linux|windows] [--profile <file>] [--misses <file>]'))
if a[:1] != ['build'] or len(a) < 2: sys.stderr.write(usage + '\\n'); sys.exit(2)
src, obj, so = a[1], a[a.index('-o') + 1], a[a.index('--shared') + 1]
windows = '--target' in a and a[a.index('--target') + 1] == 'windows'
if windows != so.endswith('.dll'): sys.stderr.write('a .dll needs --target windows\\n'); sys.exit(2)
with open(src, 'rb') as f: head = f.read(64)
with open(obj, 'wb') as f: f.write(b'object')
with open(so, 'wb') as f: f.write((b'MZ DLL pack of ' if windows else b'\\x7fELF pack of ') + head)
print('aot:', os.path.basename(src), 'packed')
'''


def nso(path, bid):   # an NSO header: the build ID at +0x40
    path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(b'NSO0' + bytes(0x3c) + bytes.fromhex(bid) + bytes(0x50))


def control(path, name, disp):   # control.nacp: the American English title at 0, the supported languages at 0x302C (en-US), the display version at 0x3060
    b = bytearray(0x4000); b[:len(name)] = name.encode(); b[0x302C:0x3030] = (1).to_bytes(4, 'little'); b[0x3060:0x3060 + len(disp)] = disp.encode()
    path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(bytes(b))


def sources(gfx):   # profile_guess.build_sources: the graphics API and no run-time shaders
    return {'graphics': (gfx, f'a {gfx} game'), **{k: (False, '') for k in ('glslc_capture', 'vk_capture', 'gl_capture', 'ue_decode', 'oodle_packs', 'ue_legacy')}}


FAKE_HOST = '''#!{py}
# a stand-in for nxhost-run: what it was started with, as JSON in $FAKE_OUT
import json, os, sys
json.dump({{'env': dict(os.environ), 'argv': sys.argv[1:], 'cwd': os.getcwd()}}, open(os.environ['FAKE_OUT'], 'w'))
'''


def launch_rows(path):
    """The launch file as a reader takes it: '# nxnative launch 1' first, then (kind, name, value) rows; # lines skipped"""
    lines = path.read_text(encoding='utf-8').splitlines()
    assert lines[0] == '# nxnative launch 1', lines[0]
    return [tuple(line.split('\t', 2)) for line in lines if line and not line.startswith('#')]


def env_txt(path):
    """The player's env.txt as dm/mac/launcher.c's env_file reads an env file (and so the launcher and run.sh's awk): blanks around the line and before the =, # comments, the value as it is
    after the =; a name a shell cannot export is skipped"""
    out = []
    for line in path.read_text(encoding='utf-8').splitlines() if path.is_file() else []:
        line = line.strip(' \t\r')
        k, eq, v = line.partition('=')
        k = k.rstrip(' \t')
        if line.startswith('#') or not eq or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', k): continue
        out.append((k, v))
    return out


def compose(launch, environ, nxpack=None):
    """What a reader of the launch file starts, by its rules: (host, pack folder, working folder, the variables it sets). The environment first, then the player's env.txt unless
    DM_SAVED_CHOICES=0, then the env rows, each only where nothing set it before"""
    rows = launch_rows(launch)
    paths = {n: v for k, n, v in rows if k == 'path'}
    aot = next((v for k, n, v in rows if k == 'pack' and (not nxpack or n == nxpack)), None)
    have, added = dict(environ), {}
    pairs = env_txt(Path(paths['choices'])) if environ.get('DM_SAVED_CHOICES') != '0' else []
    for k, v in pairs + [(n, v) for k, n, v in rows if k == 'env']:
        if k not in have: have[k] = added[k] = v
    return paths['host'], aot, paths['cwd'], added


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
        if tool == 'settings.py':   # the launcher's settings (test_settings_platforms.py tests the lists): what it was asked for, and a list of one row
            self.settings_env = {k: env.get(k) for k in ('SETTINGS_PLATFORM', 'GFXAPI', 'DM_LANG')}
            Path(cmd[-1]).write_text(json.dumps({'schema': 1, 'game': cmd[2], 'settings': [{'env': 'DM_RUMBLE', 'kind': 'toggle', 'section': 'Controls', 'label': 'Game rumble', 'note': '',
                                                                                            'default': '1', 'on': '1', 'off': '0', 'level': 'basic'}]}))
            return 0
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
        args = nsp.parse_args([str(self.nsp_dir), '-k', str(self.keys), '--platform', platform, '--nxaot', str(self.nxaot)] + list(extra))
        nsp.main(args)

    def tools(self, name): return [c for t, c in self.ran if t == name]

    @property
    def aot(self): return self.work / 'aot_v0'

    def run_sh(self, *args, **env):
        """run.sh with a stand-in for nxhost-run (NXHOST) and only PATH besides env in its environment: (exit status, stderr, what the stand-in was started with or None)"""
        host, out = self.tmp / 'bin/nxhost-run', self.tmp / 'host.json'
        if not host.is_file(): host.write_text(FAKE_HOST.format(py=sys.executable)); host.chmod(0o755)
        out.unlink(missing_ok=True)
        r = subprocess.run(['sh', str(self.work / 'run.sh'), *args], env={'PATH': os.environ['PATH'], 'NXHOST': str(host), 'FAKE_OUT': str(out), **env},
                           capture_output=True, text=True, timeout=60)
        return r.returncode, r.stderr, json.loads(out.read_text()) if out.is_file() else None

    def check_run_sh(self, nxpack=None, **env):
        """run.sh starts nxhost-run as the launch file's rules say (compose): the host, the pack folder, the working folder, every DM_ variable"""
        rc, err, got = self.run_sh('main', '--', 'arg', **env, **({'NXPACK': nxpack} if nxpack else {}))
        self.assertEqual((rc, err), (0, ''))
        host, aot, cwd, added = compose(self.work / 'launch-linux.tsv', env, nxpack)
        self.assertEqual((got['argv'], got['cwd']), ([aot, 'main', '--', 'arg'], cwd))
        self.assertEqual({k: v for k, v in got['env'].items() if k.startswith('DM_')}, {**{k: v for k, v in env.items() if k.startswith('DM_')}, **added})
        return got


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
        # the launch file: what run.sh and the launcher nxlauncher both read
        rows = launch_rows(self.work / 'launch-linux.tsv')
        self.assertEqual({n: v for k, n, v in rows if k == 'game'}, {'title': 'Test Game', 'title_id': ID, 'version': '1.0.0', 'package': 'v0', 'platform': 'linux', 'graphics': 'nvn'})
        self.assertEqual({n: v for k, n, v in rows if k == 'path'}, {'host': str(REPO / 'build/linux/nxhost-run'), 'cwd': str(self.wdir / 'run'), 'choices': str(self.wdir / 'run/env.txt'),
                                                                    'log': str(self.wdir / 'run/log.txt'), 'settings': str(self.work / 'launcher-linux.json'),
                                                                    'profile': str(self.wdir / 'profile'),   # DM_AOT_PROFILE's folder, and what packs the game again (the launcher's repack)
                                                                    'package': str(self.nsp_dir), 'packer': str(REPO / 'scripts/build_ipa_from_nsp.py'), 'python': sys.executable,
                                                                    'keys': str(self.keys), 'nxaot': str(self.nxaot)})
        self.assertEqual([(n, v) for k, n, v in rows if k == 'pack'], [('balanced', str(aot))])
        self.assertEqual([(n, v) for k, n, v in rows if k == 'env'], [
            ('DM_ROOT', str(self.work)), ('DM_ROMFS', str(self.game / 'RomFS')), ('DM_SAVE', str(self.wdir / 'run/save')), ('DM_TEMP', str(self.wdir / 'run/temp')),
            ('DM_MSL_DIR', str(self.work / 'shader/cache')), ('DM_NVN_SHMAP', str(self.prof / 'shader_map.tsv')), ('DM_LANG', 'en-US'), ('DM_WINDOW', '1'), ('DM_AUDIO', '1')])
        # the launcher's settings: settings.py's list for Linux, for this game's graphics API and packed language
        self.assertEqual(self.tools('settings.py'), [[sys.executable, str(REPO / 'dm/games/settings.py'), ID, str(self.work / 'patched_v0'), str(self.game / 'control.nacp'),
                                                      str(self.work / 'launcher-linux.json')]])
        self.assertEqual(self.settings_env, {'SETTINGS_PLATFORM': 'linux', 'GFXAPI': 'nvn', 'DM_LANG': 'en-US'})
        self.assertEqual(json.loads((self.work / 'launcher-linux.json').read_text())['game'], ID)
        # the run script reads the launch file at every start
        sh = (self.work / 'run.sh').read_text()
        self.assertTrue(os.access(self.work / 'run.sh', os.X_OK))
        self.assertIn(f'\nlaunch={self.work / "launch-linux.tsv"}\n', sh)
        self.assertTrue(sh.endswith('\nexec "${NXHOST:-$host}" "$aot" "$@"\n'))
        self.assertIn(f'-DGAME={ID}', sh); self.assertNotIn('DM_GAMES_DIR', sh)   # a registered profile
        self.assertTrue((self.wdir / 'run/save').is_dir())
        self.assertFalse((self.work / 'tmp').exists())
        got = self.check_run_sh()
        self.assertEqual(got['env']['DM_SAVE'], str(self.wdir / 'run/save'))
        # the player's choices (env.txt, as the launcher writes it, and as a player edits it) come before the env rows; what the environment sets stays
        (self.wdir / 'run/env.txt').write_text('# the player\'s choices\nDM_OPMODE=0\r\n  DM_LANG = ja  \nDM_QUOTE=it\'s "$HOME" `x`\nbad name=1\n=x\nDM_RUMBLE=0\n', encoding='utf-8')
        got = self.check_run_sh(DM_RUMBLE='1')
        self.assertEqual({k: got['env'][k] for k in ('DM_OPMODE', 'DM_LANG', 'DM_QUOTE', 'DM_RUMBLE', 'DM_WINDOW')},
                         {'DM_OPMODE': '0', 'DM_LANG': ' ja', 'DM_QUOTE': 'it\'s "$HOME" `x`', 'DM_RUMBLE': '1', 'DM_WINDOW': '1'})
        self.assertNotIn('DM_OPMODE', self.check_run_sh(DM_SAVED_CHOICES='0')['env'])   # DM_SAVED_CHOICES=0: env.txt is not read
        self.assertEqual(self.check_run_sh(DM_ROMFS='/elsewhere')['env']['DM_ROMFS'], '/elsewhere')
        rc, err, got = self.run_sh(NXPACK='max')   # no max pack yet
        self.assertEqual((rc, got), (1, None)); self.assertIn('[RED] run.sh: ', err); self.assertIn('--pack-mode max', err)
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
        # another pack mode packs everything again, into aot_v0/max/ beside links to the same input; the balanced packs stay, and both are offered, the one just packed first
        balanced = {p.name: p.read_bytes() for p in aot.iterdir() if p.is_file()}
        self.pack('linux', '--pack-mode', 'max')
        self.assertEqual([c[c.index('--mode') + 1] for c in self.tools('nxaot')], ['max', 'max'])
        self.assertEqual([Path(c[c.index('--shared') + 1]).name for c in self.tools('nxaot')], ['main.aot.so', 'nro0.aot.so'])
        self.assertEqual({p.name: p.read_bytes() for p in aot.iterdir() if p.is_file()}, balanced)
        self.assertEqual(sorted(p.name for p in (aot / 'max').iterdir()), ['.keys', 'main.aot.so', 'main.aot.tsv', 'main.image.bin', 'nro.tsv', 'nro0.aot.so'])
        for n in ('main.aot.tsv', 'main.image.bin', 'nro.tsv'): self.assertTrue(os.path.samefile(aot / n, aot / 'max' / n), n)
        self.assertEqual([(n, v) for k, n, v in launch_rows(self.work / 'launch-linux.tsv') if k == 'pack'], [('max', str(aot / 'max')), ('balanced', str(aot))])
        self.assertEqual(self.check_run_sh()['argv'][0], str(aot / 'max'))   # run.sh: the first pack row, or NXPACK
        self.assertEqual(self.check_run_sh('balanced')['argv'][0], str(aot))
        self.pack('linux'); self.assertEqual(self.tools('nxaot'), [])   # each mode keeps its own keys
        self.assertEqual([n for k, n, v in launch_rows(self.work / 'launch-linux.tsv') if k == 'pack'], ['balanced', 'max'])
        # a new image of main: the next pack of one mode makes its packs again and the other mode is no longer offered (its packs are of the old image)
        main = bytearray((self.game / 'ExeFS/main').read_bytes()); main[8] = 1; (self.game / 'ExeFS/main').write_bytes(bytes(main))   # same build ID, another image
        self.pack('linux', '--pack-mode', 'max')
        self.assertEqual([Path(c[2]).name for c in self.tools('nxaot')], ['main.aot.tsv'])
        self.assertEqual([n for k, n, v in launch_rows(self.work / 'launch-linux.tsv') if k == 'pack'], ['max'])
        self.assertEqual((aot / 'max/main.image.bin').read_bytes(), (aot / 'main.image.bin').read_bytes())   # linked again to the new input

    def test_sample_profiles(self):   # <NSP folder>/<id>-work/profile/<module>.tsv (dm/host/aot_profile.c writes them): nxaot build --profile, part of the pack's key; --no-profile
        self.pack()
        sha = (self.aot / 'main.aot.tsv').read_text().split('\n', 1)[0].split('\t')[3]
        prof = self.wdir / 'profile'; prof.mkdir(parents=True)
        self.assertIn(f'\n# export DM_AOT_PROFILE={prof}\nexec ', (self.work / 'run.sh').read_text())   # run.sh shows how to record one
        head = '# nxaot profile: guest functions sampled by the x86-64 engine (dm/host/aot_profile.c, DM_AOT_PROFILE); nxaot build --profile <this file>\n'
        (prof / 'main.tsv').write_text(head + f'# module main, build ID {BID}, image SHA-256 {sha}\n# pack x\n# <module offset of a function\'s entry> <samples>\n0x100 40\n0x200 2\n')
        (prof / 'nro0.tsv').write_text(head + f'# module nro0, build ID none, image SHA-256 {"00" * 32}\n0x10 5\n')   # recorded on another image: not used
        self.pack()
        tmp = self.work / 'tmp/aot'
        self.assertEqual(self.tools('nxaot'), [   # main's key changed with its profile; nro0's did not
            [str(self.nxaot), 'build', str(self.aot / 'main.aot.tsv'), '-o', str(tmp / 'main.o'), '--shared', str(tmp / 'main.aot.so'), '--mode', 'balanced', '--cache', str(self.wdir / 'aot-cache'),
             '--profile', str(prof / 'main.tsv')]])
        self.assertIn(f'main: sample profile {prof / "main.tsv"} (42 samples at 2 functions): those at tier H, the others at C', self.said)
        self.assertTrue(any(m.startswith(f'the sample profile {prof / "nro0.tsv"} was recorded on another image of nro0') for m in self.said), self.said)
        self.pack(); self.assertEqual(self.tools('nxaot'), [])   # reused while it is unchanged
        self.pack('linux', '--pack-mode', 'max')
        self.assertEqual([c[-2:] for c in self.tools('nxaot')], [['--profile', str(prof / 'main.tsv')], ['--cache', str(self.wdir / 'aot-cache')]])
        self.assertIn(f'main: sample profile {prof / "main.tsv"} (42 samples at 2 functions): those at tier H+, the others at H', self.said)
        self.pack('linux', '--no-profile', '--pack-mode', 'max')   # ignored: main packed again without it
        self.assertEqual([Path(c[2]).name for c in self.tools('nxaot')], ['main.aot.tsv'])
        self.assertEqual([c for c in self.tools('nxaot') if '--profile' in c], [])
        self.assertIn(f'--no-profile: the sample profiles in {prof} are not used', self.said)
        (prof / 'main.tsv').write_text('0x100 many\n')   # one that does not parse: packed without it, said
        self.pack('linux', '--pack-mode', 'max'); self.assertEqual(self.tools('nxaot'), [])
        self.assertTrue(any(m.startswith(f'[RED] the sample profile {prof / "main.tsv"} does not parse') for m in self.said), self.said)
        with self.assertRaisesRegex(nsp.BuildError, '--no-profile make the packs'):
            nsp.main(nsp.parse_args([str(self.nsp_dir), '-k', str(self.keys), '--platform', 'ios', '--no-profile']))

    def test_jit_misses(self):   # <NSP folder>/<id>-work/profile/<module>.misses.tsv (dm/host/aot_profile.c writes them beside the profile): nxaot build --misses, their addresses in the pack's key
        self.pack()
        sha = (self.aot / 'main.aot.tsv').read_text().split('\n', 1)[0].split('\t')[3]
        prof = self.wdir / 'profile'; prof.mkdir(parents=True)
        head = ('# nxaot misses: guest addresses the x86-64 engine\'s JIT fallback ran because the pack had no compiled code there (dm/host/aot_run.c, DM_AOT_PROFILE); '
                'nxaot build --misses <this file>\n')
        def misses(name, image, rows):   # the host's form (t_aot_module's step 7 reads it back)
            (prof / f'{name}.misses.tsv').write_text(head + f'# module {name}, build ID {BID}, image SHA-256 {image}\n# pack x\n# <module offset> <reason> <entries>\n'
                                                     + ''.join(f'{o:#x} {r} {n}\n' for o, r, n in rows))
        misses('main', sha, [(0x208, 'indirect', 1), (0x100, 'nonlocal', 2)])
        misses('nro0', '00' * 32, [(0x10, 'call', 1)])   # recorded on another image: not used
        self.pack()
        tmp = self.work / 'tmp/aot'
        self.assertEqual(self.tools('nxaot'), [   # main's key changed with its misses; nro0's did not
            [str(self.nxaot), 'build', str(self.aot / 'main.aot.tsv'), '-o', str(tmp / 'main.o'), '--shared', str(tmp / 'main.aot.so'), '--mode', 'balanced', '--cache', str(self.wdir / 'aot-cache'),
             '--misses', str(prof / 'main.misses.tsv')]])
        self.assertIn(f'main: JIT misses {prof / "main.misses.tsv"} (2 addresses the game ran in the JIT fallback): roots of discovery for its pack', self.said)
        self.assertTrue(any(m.startswith(f'the JIT misses {prof / "nro0.misses.tsv"} were recorded on another image of nro0') for m in self.said), self.said)
        self.assertEqual([n for k, n, v in launch_rows(self.work / 'launch-linux.tsv') if k == 'pack'], ['balanced'])   # complete with them
        misses('main', sha, [(0x208, 'indirect', 9), (0x100, 'nonlocal', 4)])   # a later session: the same addresses, other counts
        self.pack(); self.assertEqual(self.tools('nxaot'), [])                      # the counts are in no key
        misses('main', sha, [(0x208, 'indirect', 9), (0x100, 'nonlocal', 4), (0x300, 'host', 1)])   # a new address: packed again
        self.pack(); self.assertEqual([c[-2:] for c in self.tools('nxaot')], [['--misses', str(prof / 'main.misses.tsv')]])
        self.pack('linux', '--no-profile'); self.assertEqual(self.tools('nxaot'), [])   # --no-profile leaves the misses in use: the same key
        (prof / 'main.tsv').write_text(head.replace('misses', 'profile') + f'# module main, build ID {BID}, image SHA-256 {sha}\n0x100 40\n')   # beside a profile: both
        self.pack(); self.assertEqual([c[-4:] for c in self.tools('nxaot')], [['--profile', str(prof / 'main.tsv'), '--misses', str(prof / 'main.misses.tsv')]])
        (prof / 'main.misses.tsv').write_text('0x208 indirect many\n')   # one that does not parse: packed without it, said
        self.pack(); self.assertEqual([c[-2:] for c in self.tools('nxaot')], [['--profile', str(prof / 'main.tsv')]])
        self.assertTrue(any(m.startswith(f'[RED] the JIT misses {prof / "main.misses.tsv"} do not parse') for m in self.said), self.said)

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
        self.assertEqual(nsp.nxaot_options(self.nxaot, dict(os.environ)), {'--cache', '--workers', '--target', '--profile', '--misses'})
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
        envs = {n: v for k, n, v in launch_rows(self.work / 'launch-linux.tsv') if k == 'env'}
        self.assertEqual(envs['DM_MSL_DIR'], str(self.work / 'shader')); self.assertNotIn('DM_NVN_SHMAP', envs)
        self.assertEqual(self.settings_env['GFXAPI'], 'gl')
        self.assertEqual(self.check_run_sh()['env']['DM_MSL_DIR'], str(self.work / 'shader'))
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
        self.assertIn(('env', 'DM_NVN_SHMAP', str(pdir / ID / 'shader_map.tsv')), launch_rows(self.work / 'launch-linux.tsv'))
        self.pack()
        self.assertEqual((self.rewrites, self.tools('nxaot')), ([], []))

    def test_env_file_switches_in_the_launch_file(self):   # --env-file: env rows before the layout's, winning over its DM_LANG; the player's env.txt still wins over them
        envf = self.tmp / 'switches.txt'; envf.write_text('# diagnostics\nDM_MOTION=0\nDM_LANG=ja\n')
        self.pack('linux', '--env-file', str(envf))
        envs = [(n, v) for k, n, v in launch_rows(self.work / 'launch-linux.tsv') if k == 'env']
        self.assertEqual(envs[:3], [('DM_MOTION', '0'), ('DM_LANG', 'ja'), ('DM_ROOT', str(self.work))])
        self.assertEqual([n for n, _ in envs].count('DM_LANG'), 1)
        self.assertEqual(self.check_run_sh()['env']['DM_LANG'], 'ja')
        (self.wdir / 'run/env.txt').write_text('DM_MOTION=1\n')
        self.assertEqual(self.check_run_sh()['env']['DM_MOTION'], '1')
        envf.write_text('bad name=1\n')
        with self.assertRaisesRegex(nsp.BuildError, "sets 'bad name', which is not a variable name"): self.pack('linux', '--env-file', str(envf))
        envf.write_text('DM_X=a\tb\n')
        with self.assertRaisesRegex(nsp.BuildError, 'holds a tab or a line break'): self.pack('linux', '--env-file', str(envf))

    def test_options_belong_to_the_port(self):
        with self.assertRaisesRegex(nsp.BuildError, '--platform linux / windows'):
            nsp.main(nsp.parse_args([str(self.nsp_dir), '-k', str(self.keys), '--platform', 'ios', '--pack-mode', 'max']))
        with self.assertRaisesRegex(nsp.BuildError, '-o names the IPA'):
            nsp.main(nsp.parse_args([str(self.nsp_dir), '-k', str(self.keys), '--platform', 'linux', '-o', str(self.tmp / 'x.ipa')]))


class Windows(Pack):
    def test_dlls_run_cmd_and_reuse(self):
        self.pack('windows')
        aot, nro_dir, tmp = self.aot, self.work / 'nro_v0', self.work / 'tmp/aot'
        # nxaot with --target windows: the manifest and the NRO file into DLLs beside them, the COFF object in the temporary folder, the cache in the work folder
        self.assertEqual(self.tools('nxaot'), [
            [str(self.nxaot), 'build', str(aot / 'main.aot.tsv'), '-o', str(tmp / 'main.obj'), '--shared', str(tmp / 'main.aot.dll'), '--mode', 'balanced', '--target', 'windows',
             '--cache', str(self.wdir / 'aot-cache')],
            [str(self.nxaot), 'build', str(nro_dir / 'nro0.nro'), '-o', str(tmp / 'nro0.obj'), '--shared', str(tmp / 'nro0.aot.dll'), '--mode', 'balanced', '--target', 'windows',
             '--cache', str(self.wdir / 'aot-cache')]])
        self.assertEqual(sorted(p.name for p in aot.iterdir()), ['.keys-windows', 'main.aot.dll', 'main.aot.tsv', 'main.image.bin', 'nro.tsv', 'nro0.aot.dll'])
        self.assertTrue((aot / 'main.aot.dll').read_bytes().startswith(b'MZ DLL pack of # nxaot-manifest 1'))
        self.assertEqual((aot / 'nro0.aot.dll').read_bytes(), b'MZ DLL pack of ' + self.nro[:64])
        self.assertFalse((self.work / 'tmp').exists())
        self.assertEqual(len(self.tools('dmsh_build_vk.py')), 1)
        self.assertTrue(any(m.startswith('recompiler: ') and m.endswith(', DLLs: --target windows)') for m in self.said))
        self.assertTrue(any(m.startswith('done: the windows pack of ') for m in self.said))
        # the launch file: the Windows host program, the DLL packs' folder; settings.py's list for Windows
        rows = launch_rows(self.work / 'launch-windows.tsv')
        self.assertIn(('path', 'host', str(REPO / 'build/win64/nxhost-run.exe')), rows); self.assertIn(('game', 'platform', 'windows'), rows)
        self.assertEqual([r for r in rows if r[0] == 'pack'], [('pack', 'balanced', str(aot))])
        self.assertIn(('env', 'DM_SAVE', str(self.wdir / 'run/save')), rows)
        self.assertEqual(self.settings_env['SETTINGS_PLATFORM'], 'windows')
        self.assertTrue((self.work / 'launcher-windows.json').is_file())
        # run.cmd reads it (cmd.exe does not run here: the text only), CRLF lines; for /f splits the rows at the tab
        cmd = (self.work / 'run.cmd').read_bytes().decode()
        self.assertTrue(cmd.startswith('@echo off\r\n')); self.assertNotIn('\n', cmd.replace('\r\n', ''))
        self.assertIn(f'\r\nset "NXRUN_LAUNCH={self.work / "launch-windows.tsv"}"\r\n', cmd)
        self.assertEqual(cmd.count('for /f "usebackq eol=# tokens=1,2,* delims=\t" %%a in ("%NXRUN_LAUNCH%") do'), 2)
        self.assertIn('\r\nif defined NXHOST set "NXRUN_host=%NXHOST%"\r\n', cmd)
        self.assertIn('\r\n"%NXRUN_host%" "%NXRUN_AOT%" %*\r\nset "NXRUN_RC=%errorlevel%"\r\n:nxrun_end\r\nif defined NXRUN_CP chcp %NXRUN_CP% > nul\r\nexit /b %NXRUN_RC%\r\n', cmd)
        self.assertIn('x86_64-w64-mingw32.cmake', cmd)
        self.assertIn(f'\r\nrem set "DM_AOT_PROFILE={self.wdir / "profile"}"\r\n', cmd)
        # a second run reuses every step
        first = {p.name: p.read_bytes() for p in aot.iterdir() if p.is_file()}
        self.pack('windows')
        self.assertEqual((self.rewrites, self.tools('nxaot'), self.tools('dmsh_build_vk.py')), ([], [], []))
        self.assertEqual({p.name: p.read_bytes() for p in aot.iterdir() if p.is_file()}, first)
        # a Linux pack of the same folder keeps its own keys: neither undoes the other's packs
        self.pack('linux')
        self.assertEqual([Path(c[c.index('--shared') + 1]).name for c in self.tools('nxaot')], ['main.aot.so', 'nro0.aot.so'])
        self.assertEqual(sorted(p.name for p in aot.iterdir() if p.name.startswith('.') or 'aot.' in p.name and not p.name.endswith('.tsv')),
                         ['.keys', '.keys-windows', 'main.aot.dll', 'main.aot.so', 'nro0.aot.dll', 'nro0.aot.so'])
        self.pack('windows'); self.assertEqual(self.tools('nxaot'), [])
        self.pack('linux'); self.assertEqual(self.tools('nxaot'), [])

    def test_nxaot_without_target_stops_at_the_start(self):
        with mock.patch.dict(os.environ, {'FAKE_NXAOT_OLD': '1'}):
            with self.assertRaisesRegex(nsp.BuildError, 'writes no Windows packs .*no --target option'): self.pack('windows')
        self.assertEqual((self.ran, self.rewrites), ([], []))   # before any long step (nxaot_options runs nxaot itself for its usage)
        self.assertFalse(self.aot.exists())


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
        self.assertIn('the packs are DLLs (nxaot build --target windows)', notes[0]); self.assertIn('nro.tsv', notes[1]); self.assertIn('(<name>.aot.dll)', notes[1])
        self.assertIn('dmsh_build_vk.py', notes[2])
        self.assertNotIn('DLL', nsp.port_notes('linux', S, ro=True)[0]); self.assertIn('(<name>.aot.so)', nsp.port_notes('linux', S, ro=True)[1])
        self.assertTrue(notes[3].startswith('the GLSLC capture boots the game in the Mac host, which windows does not have'))


class ProfileDirs(unittest.TestCase):   # DM_GAMES_DIR as the script joins it (os.pathsep), read by game.py: on Windows a ';' list of paths with drive letters
    def test_split_on_pathsep(self):
        with mock.patch.object(game.os, 'pathsep', ';'), mock.patch.dict(os.environ, {'DM_GAMES_DIR': r'C:\games\a_build\profile;;D:\more'}):
            self.assertEqual(game.extra_dirs(), [Path(r'C:\games\a_build\profile'), Path(r'D:\more')], 'a drive letter\'s colon is no separator')

    def test_profile_found_in_the_second_folder(self):
        tmp = Path(tempfile.mkdtemp()).resolve(); self.addCleanup(shutil.rmtree, tmp)
        (tmp / 'one').mkdir(); (tmp / 'two' / ID).mkdir(parents=True); (tmp / 'two' / ID / 'game.tsv').write_text(f'title_id\t{ID}\n')
        joined = os.pathsep.join([str(tmp / 'one'), str(tmp / 'two')])   # as build_ipa_from_nsp.py writes it
        with mock.patch.dict(os.environ, {'DM_GAMES_DIR': joined}):
            self.assertEqual(game.extra_dirs(), [tmp / 'one', tmp / 'two'])
            self.assertEqual(game.find_dir(ID), tmp / 'two' / ID)


if __name__ == '__main__':
    unittest.main()
