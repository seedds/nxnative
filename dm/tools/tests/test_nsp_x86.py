"""NSP script packs for Windows and Linux (scripts/build_ipa_from_nsp.py --platform linux / windows): the recompiler's input (patch_nso.py --aot, one run beside Apple's outputs) and nro.tsv in
<id>_build/aot_<version>/, nxaot build of every module and NRO into <name>.aot.so (Linux) or with --target windows into <name>.aot.dll (Windows), both pack modes side by side (max in
aot_<version>/max/), the Vulkan shader steps (dmsh_build_vk.py, gl_shaders.py --vk, none for a guest Vulkan game), the launcher's settings (launcher-<platform>.json: settings.py's call; test_settings_platforms.py tests the lists),
the game profile's file (game-profile-<platform>.tsv: the real game.py runtime, of the registered and of a derived profile, refreshed when the profile changes), the launch file
(launch-<platform>.tsv) and the run script that reads it: run.sh is run with a stand-in for nxhost-run and checked against the launch file's rules (--profile with the game profile's
file, the environment, then the player's env.txt, then the env rows; the bundle's one host bin/nxhost-run), an nxaot without --target stopping a Windows pack at the start, and the
reuse of every step on a second run. Real fingerprints, a fake nxaot that
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
import game, make_ext_font, nacp, profile_guess
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
nsp.pg, nsp.nacp = profile_guess, nacp   # main sets them

TID = 0x0100AAAA00000000
ID = f'{TID:016X}'
BID, BID2 = 'ab' * 16, 'cd' * 16
REAL_POPEN, REAL_RUN, TRUE = subprocess.Popen, nsp.run, shutil.which('true')   # TRUE: what every tool the script looks for on PATH is found as
REAL_BUILD_NX_EXTRACT, REAL_BUNDLED_TOOL = nsp.build_nx_extract, nsp.bundled_tool
NXAOT ='''#!{py}
# a stand-in for nxaot: `nxaot build` with no input prints the usage (exit 2); a build writes the object and the pack (a DLL with --target windows)
import os, sys
a = sys.argv[1:]
usage = ('usage: nxaot build <module.nro | name.aot.tsv> -o <object.o> [--shared <lib.so>] [--mode balanced|max]'
         + ('' if os.environ.get('FAKE_NXAOT_OLD') else ' [--workers N] [--cache <dir>] [--target linux|windows] [--profile <file>] [--misses <file>] [--bind <file>]'))
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
# a stand-in for nxhost-run: what it was started with, as JSON in $FAKE_OUT (self: the program path it was started as)
import json, os, sys
json.dump({{'env': dict(os.environ), 'argv': sys.argv[1:], 'cwd': os.getcwd(), 'self': sys.argv[0]}}, open(os.environ['FAKE_OUT'], 'w'))
'''


def launch_rows(path):
    """The launch file as a reader takes it: '# nxnative launch 1' first, then (kind, name, value) rows; # lines skipped"""
    lines = path.read_text(encoding='utf-8').splitlines()
    assert lines[0] == '# nxnative launch 1', lines[0]
    return [tuple(line.split('\t', 2)) for line in lines if line and not line.startswith('#')]


def made(path):
    """The launch file's rows of what the packs were made with (made, samples, misses, targets: made_rows), which the launcher compares with the profile folder"""
    return [r for r in launch_rows(path) if r[0] in ('made', 'samples', 'misses', 'targets')]


# The digests the launcher checks too (dm/launcher/tests/t_launcher_model.cpp holds the same constants): FNV-1a 64 of main.tsv's offsets 0x100 and 0x200 ('100\n200\n'), and of the
# misses' pairs (0x100 nonlocal) and (0x208 indirect) ('100 nonlocal\n208 indirect\n')
DIGEST_OFFSETS, DIGEST_MISSES = 'ce9643fb137f8dce', 'b3c6eacf4fcf4a0e'
# The known targets' fixture, the same rows in t_launcher_model.cpp (kTargetRows), and the dominant targets nxaot's rule gives them (dm/tools/aot/src/build/pack.cpp
# TargetsFromProfile: at least 90 % of a site's binds): 0x1a64 its only target; 0x1984 none (2, 1, 2: the best 40 %); 0x2000 0x3000 (18 of 20, 90 %); 0x2100 none (17 of 19,
# 89 %); 0x2200 no site (0 binds); 0x2300 0x3300 (a repeated row, 5 + 4). DIGEST_TARGETS: FNV-1a 64 of '1a64:1820\n2000:3000\n2300:3300\n'
TARGET_ROWS = [(0x1a64, 0x1820, 1), (0x1984, 0x1690, 2), (0x1984, 0x16d0, 1), (0x1984, 0x1720, 2), (0x2000, 0x3000, 18), (0x2000, 0x3100, 2), (0x2100, 0x3000, 17),
               (0x2100, 0x3100, 2), (0x2200, 0x3200, 0), (0x2300, 0x3300, 5), (0x2300, 0x3300, 4)]
DOMINANT, DIGEST_TARGETS = ((0x1a64, 0x1820), (0x2000, 0x3000), (0x2300, 0x3300)), 'fca3e8e0f0106c26'


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
    """What a reader of the launch file starts, by its rules: (host, its arguments before the script's: --profile <the game profile's file> <pack folder>, working folder, the
    variables it sets). The environment first, then the player's env.txt unless DM_SAVED_CHOICES=0, then the env rows, each only where nothing set it before; an empty value
    counts as unset on every platform (dm/host/plat/plat.h), in the environment and on a line of env.txt"""
    rows = launch_rows(launch)
    paths = {n: v for k, n, v in rows if k == 'path'}
    aot = next((v for k, n, v in rows if k == 'pack' and (not nxpack or n == nxpack)), None)
    have, added = {k: v for k, v in environ.items() if v}, {}
    pairs = env_txt(Path(paths['choices'])) if environ.get('DM_SAVED_CHOICES') != '0' else []
    for k, v in pairs + [(n, v) for k, n, v in rows if k == 'env']:
        if v and k not in have: have[k] = added[k] = v
    return paths['host'], ['--profile', paths['game_profile'], aot], paths['cwd'], added


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
                                                       'mem_reserve\t0x180000000', 'mem_alias\t0x100000000', 'mem_stack\t0x100000', 'app_pool\t0xcd500000',   # game.py's constants
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
        self.said, self.ran, self.rewrites, self.targets_switch, self.tool_env, self.vk_tools_bad = [], [], [], [], {}, False
        self.gfx = 'nvn'
        self.letters = self.tmp / 'fonts/Letters-Bold.ttf'; self.letters.parent.mkdir(); self.letters.write_bytes(b'letter font')   # the system's letter font (make_ext_font.letter_font)
        fam = {'base': self.nsp_dir / 'base.nsp', 'updates': [], 'dlc': []}
        # capstone: a stand-in of a version the packer takes (dm/tools/capstone_version.py; its prerequisite check reads the version), whatever this Python has
        for cm in (mock.patch.dict(os.environ, {'DM_GAMES_DIR': str(self.tmp / 'games'), 'XLATE_VK': str(self.dll)}), mock.patch.dict(sys.modules, {'capstone': types.SimpleNamespace(__version__='5.0.9')}),
                   mock.patch.object(make_ext_font, 'letter_font', side_effect=lambda *a: str(self.letters) if self.letters.is_file() else None),
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
        self.ran.append((tool, cmd)); self.tool_env[tool] = dict(env or {})
        if tool == 'nxaot':   # the fake nxaot, run for real; whether the packer kept the targets file from it (NXAOT_PROFILE_TARGETS)
            self.targets_switch.append((env or os.environ).get('NXAOT_PROFILE_TARGETS'))
            return REAL_RUN(cmd, log, env=env, cwd=cwd, on_line=on_line, ok=ok)
        if tool == 'game.py':   # game.py runtime, run for real: the profile file of the profile DM_GAMES_DIR (and DM_GAMES_FIRST, a derived one) in env finds
            return REAL_RUN(cmd, log, env=env, cwd=cwd, on_line=on_line, ok=ok)
        if tool == 'settings.py':   # the launcher's settings (test_settings_platforms.py tests the lists): what it was asked for, and a list of one row
            self.settings_env = {k: env.get(k) for k in ('SETTINGS_PLATFORM', 'GFXAPI', 'DM_LANG')}
            Path(cmd[-1]).write_text(json.dumps({'schema': 1, 'game': cmd[2], 'settings': [{'env': 'DM_RUMBLE', 'kind': 'toggle', 'section': 'Controls', 'label': 'Game rumble', 'note': '',
                                                                                            'default': '1', 'on': '1', 'off': '0', 'level': 'basic'}]}))
            return 0
        if tool == 'nro_collect.py':   # the registered NRO, found loose: nro.tsv as nro_collect.py writes it (a refused online SDK too)
            out = Path(cmd[3]); out.mkdir(parents=True); (out / 'nro0.nro').write_bytes(self.nro)
            (out / 'nro.tsv').write_text('# name\tsha256\tsize\tnrr\twhere\n' + f'nro0\t{hashlib.sha256(self.nro).hexdigest()}\t{len(self.nro)}\tgame.nrr\tloose a.nro\n'
                                         + f'refused\t{"ef" * 32}\t256\tgame.nrr\tloose eos.nro (EOS_Platform_Create)\n')
        elif tool in ('dmsh_chain.py', 'dmsh_chain_vk.py'):   # --header: the repository's headers are not written by a test; --check-tools: the tools match the lock, unless vk_tools_bad
            if '--check-tools' in cmd and self.vk_tools_bad:
                log.write_text('[RED] Vulkan translation chain tools do not match the lock file: spirv-cross_linux-x64: lock vulkan-sdk-1.4.363.0 / actual missing\n'); return 1
        elif tool == 'dmsh_build_vk.py':   # a full build: the library, its stamp and the profile's tables naming it
            lib = Path(cmd[3]) / f'vk-{nsp.chain_vk_of(REPO)}'; lib.mkdir(parents=True, exist_ok=True); (lib / '0123456789abcdef.spv').write_bytes(b'spv')
            (lib / 'inputs.tsv').write_text(f'# stamp\ngame\t{ID}\ninputs\tdigest\n')
            for n in nsp.CHAIN_TABLES_VK:   # into the profile DM_GAME names now (a derived one comes first)
                (game.load(ID).dir / n).write_text(f'# chain {lib.name} inputs digest 2026-11-01T00:00:00+00:00\n')
            log.write_text('shaders 1: ok 1\nshader cache: hit 0, built 1\n')
        elif tool == 'make_ext_font.py':   # the button-icon font, from the letter font given
            Path(cmd[2]).write_bytes(b'ttf with letters of ' + Path(cmd[3]).read_bytes())
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
        self.ran.clear(); self.rewrites.clear(); self.said.clear(); self.targets_switch.clear()
        args = nsp.parse_args([str(self.nsp_dir), '-k', str(self.keys), '--platform', platform, '--nxaot', str(self.nxaot)] + list(extra))
        nsp.main(args)

    def tools(self, name): return [c for t, c in self.ran if t == name]

    @property
    def aot(self): return self.work / 'aot_v0'

    def run_sh(self, *args, **env):
        """run.sh with a stand-in for nxhost-run (NXHOST) and only PATH besides env in its environment: (exit status, stderr, what the stand-in was started with or None)"""
        host, out = self.tmp / 'nxhost/nxhost-run', self.tmp / 'host.json'
        if not host.is_file(): host.parent.mkdir(exist_ok=True); host.write_text(FAKE_HOST.format(py=sys.executable)); host.chmod(0o755)
        out.unlink(missing_ok=True)
        r = subprocess.run(['sh', str(self.work / 'run.sh'), *args], env={'PATH': os.environ['PATH'], 'NXHOST': str(host), 'FAKE_OUT': str(out), **env},
                           capture_output=True, text=True, timeout=60)
        return r.returncode, r.stderr, json.loads(out.read_text()) if out.is_file() else None

    def check_run_sh(self, nxpack=None, **env):
        """run.sh starts nxhost-run as the launch file's rules say (compose): the host, the pack folder, the working folder, every DM_ variable"""
        rc, err, got = self.run_sh('main', '--', 'arg', **env, **({'NXPACK': nxpack} if nxpack else {}))
        self.assertEqual((rc, err), (0, ''))
        host, args, cwd, added = compose(self.work / 'launch-linux.tsv', env, nxpack)
        self.assertEqual((got['argv'], got['cwd']), (args + ['main', '--', 'arg'], cwd))
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
        self.assertEqual({n: v for k, n, v in rows if k == 'path'}, {'host': str(REPO / 'build/linux/nxhost-run'), 'game_profile': str(self.work / 'game-profile-linux.tsv'),
                                                                    'cwd': str(self.wdir / 'run'), 'choices': str(self.wdir / 'run/env.txt'),
                                                                    'log': str(self.wdir / 'run/log.txt'), 'settings': str(self.work / 'launcher-linux.json'),
                                                                    'profile': str(self.wdir / 'profile'),   # DM_AOT_PROFILE's folder, and what packs the game again (the launcher's repack)
                                                                    'package': str(self.nsp_dir), 'packer': str(REPO / 'scripts/build_ipa_from_nsp.py'), 'python': sys.executable,
                                                                    'keys': str(self.keys), 'nxaot': str(self.nxaot)})
        self.assertEqual([(n, v) for k, n, v in rows if k == 'pack'], [('balanced', str(aot))])
        self.assertEqual([(n, v) for k, n, v in rows if k == 'env'], [
            ('DM_ROOT', str(self.work)), ('DM_ROMFS', str(self.game / 'RomFS')), ('DM_SAVE', str(self.wdir / 'run/save')), ('DM_TEMP', str(self.wdir / 'run/temp')),
            ('DM_MSL_DIR', str(self.work / 'shader/cache')), ('DM_FONT_DIR', str(self.work / 'font')), ('DM_NVN_SHMAP', str(self.prof / 'shader_map.tsv')), ('DM_LANG', 'en-US'),
            ('DM_WINDOW', '1'), ('DM_AUDIO', '1')])
        # the button-icon font: make_ext_font.py with the system's letter font, into <id>_build/font, which DM_FONT_DIR names
        self.assertEqual(self.tools('make_ext_font.py'), [[sys.executable, str(REPO / 'dm/tools/make_ext_font.py'), str(self.work / 'font/nintendo_ext.ttf'), str(self.letters)]])
        self.assertEqual((self.work / 'font/nintendo_ext.ttf').read_bytes(), b'ttf with letters of letter font')
        self.assertIn(f'button-icon font: {self.work / "font/nintendo_ext.ttf"} (letters from {self.letters})', self.said)
        # the launcher's settings: settings.py's list for Linux, for this game's graphics API and packed language
        self.assertEqual(self.tools('settings.py'), [[sys.executable, str(REPO / 'dm/games/settings.py'), ID, str(self.work / 'patched_v0'), str(self.game / 'control.nacp'),
                                                      str(self.work / 'launcher-linux.json')]])
        self.assertEqual(self.settings_env, {'SETTINGS_PLATFORM': 'linux', 'GFXAPI': 'nvn', 'DM_LANG': 'en-US'})
        self.assertEqual(json.loads((self.work / 'launcher-linux.json').read_text())['game'], ID)
        # the run script reads the launch file at every start
        sh = (self.work / 'run.sh').read_text()
        self.assertTrue(os.access(self.work / 'run.sh', os.X_OK))
        self.assertIn(f'\nlaunch={self.work / "launch-linux.tsv"}\n', sh)
        self.assertTrue(sh.endswith('\nexec "${NXHOST:-$host}" --profile "$gprof" "$aot" "$@"\n'))
        self.assertIn('-DGAME=<any title ID of dm/games>', sh); self.assertNotIn('DM_GAMES_DIR', sh); self.assertNotIn('hosts/', sh)   # one host for every profile
        # the game profile's file: game.py runtime of the profile, beside the launch file
        self.assertEqual(self.tools('game.py'), [[sys.executable, str(REPO / 'dm/games/game.py'), 'runtime', ID, str(self.work / 'game-profile-linux.tsv')]])
        self.assertEqual((self.work / 'game-profile-linux.tsv').read_text(encoding='utf-8'), game.runtime(game.load(ID)))
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
        # an empty value is unset, as on Windows (whose C runtime cannot hold one): a line of env.txt with one is as if absent, and one in the environment keeps nothing out
        (self.wdir / 'run/env.txt').write_text('DM_LANG=\nDM_OPMODE=0\n', encoding='utf-8')
        rc, err, got = self.run_sh(DM_WINDOW='')
        self.assertEqual((rc, err), (0, ''))
        self.assertEqual((got['env'].get('DM_LANG'), got['env'].get('DM_WINDOW'), got['env'].get('DM_OPMODE')), ('en-US', '1', '0'))
        self.assertEqual(self.check_run_sh(DM_ROMFS='/elsewhere')['env']['DM_ROMFS'], '/elsewhere')
        rc, err, got = self.run_sh(NXPACK='max')   # no max pack yet
        self.assertEqual((rc, got), (1, None)); self.assertIn('[RED] run.sh: ', err); self.assertIn('--pack-mode max', err)
        # a second run reuses every step: no rewrite, no pack, no shader build, no font
        first = {p.name: p.read_bytes() for p in aot.iterdir() if p.is_file()}
        self.pack()
        self.assertEqual((self.rewrites, self.tools('nxaot'), self.tools('dmsh_build_vk.py'), self.tools('make_ext_font.py')), ([], [], [], []))
        self.assertEqual({p.name: p.read_bytes() for p in aot.iterdir() if p.is_file()}, first)
        self.letters.write_bytes(b'another letter font'); self.pack()   # the letter font changed: the font is made again, and only it
        self.assertEqual((len(self.tools('make_ext_font.py')), self.rewrites, self.tools('nxaot')), (1, [], []))
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
        self.assertEqual(self.check_run_sh()['argv'][2], str(aot / 'max'))   # run.sh: the first pack row, or NXPACK
        self.assertEqual(self.check_run_sh('balanced')['argv'][2], str(aot))
        self.pack('linux'); self.assertEqual(self.tools('nxaot'), [])   # each mode keeps its own keys
        self.assertEqual([n for k, n, v in launch_rows(self.work / 'launch-linux.tsv') if k == 'pack'], ['balanced', 'max'])
        # a new image of main: the next pack of one mode makes its packs again and the other mode is no longer offered (its packs are of the old image)
        main = bytearray((self.game / 'ExeFS/main').read_bytes()); main[8] = 1; (self.game / 'ExeFS/main').write_bytes(bytes(main))   # same build ID, another image
        self.pack('linux', '--pack-mode', 'max')
        self.assertEqual([Path(c[2]).name for c in self.tools('nxaot')], ['main.aot.tsv'])
        self.assertEqual([n for k, n, v in launch_rows(self.work / 'launch-linux.tsv') if k == 'pack'], ['max'])
        self.assertEqual((aot / 'max/main.image.bin').read_bytes(), (aot / 'main.image.bin').read_bytes())   # linked again to the new input

    def test_game_profile_file(self):   # <id>_build/game-profile-linux.tsv: what the one host of every game takes with --profile; refreshed by a repack; run.sh refuses to start without it
        self.pack()
        gprof = self.work / 'game-profile-linux.tsv'
        self.assertIn(('path', 'game_profile', str(gprof)), launch_rows(self.work / 'launch-linux.tsv'))
        rows = dict(l.split('\t', 1) for l in gprof.read_text(encoding='utf-8').splitlines() if l and not l.startswith('#'))
        self.assertEqual((rows['id'], rows['name'], rows['modules'], rows['display_versions'], rows['fps_target']), (ID, 'Test Game', 'main', f'{BID}=1.0.0', '0'))
        self.assertEqual(self.check_run_sh()['argv'][:2], ['--profile', str(gprof)])
        # the environment's DM_GAME_PROFILE does not replace it: the command line names the pack's file (game_rt.c: --profile wins over the switch)
        self.assertEqual(self.check_run_sh(DM_GAME_PROFILE='/elsewhere.tsv')['argv'][:2], ['--profile', str(gprof)])
        # the profile's game.tsv changes: the next pack (a repack) writes the file again, and the packs, which do not depend on it, are reused
        (self.prof / 'game.tsv').write_text((self.prof / 'game.tsv').read_text() + 'fps_target\t30\n')
        self.pack()
        self.assertEqual(self.tools('nxaot'), [])
        self.assertIn('fps_target\t30\n', gprof.read_text(encoding='utf-8'))
        self.assertEqual(gprof.read_text(encoding='utf-8'), game.runtime(game.load(ID)))
        # the file gone: run.sh says so in a red line and starts nothing (the host would run another game's profile)
        gprof.unlink()
        rc, err, got = self.run_sh('main')
        self.assertEqual((rc, err, got), (1, f'[RED] run.sh: the game profile {gprof} is missing: pack the game again\n', None))
        # a launch file without the row (a pack made before the one host): the same
        launch = self.work / 'launch-linux.tsv'
        launch.write_text(''.join(l for l in launch.read_text(encoding='utf-8').splitlines(True) if not l.startswith('path\tgame_profile\t')), encoding='utf-8')
        rc, err, got = self.run_sh('main')
        self.assertEqual((rc, got), (1, None)); self.assertIn('[RED] run.sh: ', err); self.assertIn('names no host, game profile, working folder or pack', err)
        # a profile game.py refuses stops the pack with game.py's own words, before the packs
        (self.prof / 'game.tsv').write_text((self.prof / 'game.tsv').read_text().replace('fps_target\t30\n', 'fps_target\t45\n'))
        with self.assertRaisesRegex(nsp.BuildError, r'game\.py exit code 1 \(full output: .*game_profile_linux\.log\)'): self.pack()
        self.assertIn('fps_target must be 0, 30 or 60: 45', (self.work / 'logs/game_profile_linux.log').read_text())

    def test_sample_profiles(self):   # <NSP folder>/<id>-work/profile/<module>.tsv (dm/host/aot_profile.c writes them): nxaot build --profile, part of the pack's key; --no-profile
        self.pack()
        sha = (self.aot / 'main.aot.tsv').read_text().split('\n', 1)[0].split('\t')[3]
        launch = self.work / 'launch-linux.tsv'
        self.assertEqual(made(launch), [('made', 'profile', 'yes'), ('made', 'misses', 'yes'), ('made', 'targets', 'yes')])   # no profile yet: no module's row
        self.assertEqual(nsp.fnv64(''), 'cbf29ce484222325'); self.assertEqual(nsp.fnv64('a'), 'af63dc4c8601ec8c')   # FNV-1a 64's offset basis and its test vector
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
        # the launch file records what main's packs took (nro0's profile of another image: none), for the launcher's "a better pack is possible"
        self.assertEqual(made(launch), [('made', 'profile', 'yes'), ('made', 'misses', 'yes'), ('made', 'targets', 'yes'), ('samples', 'main', f'2 42 {DIGEST_OFFSETS}')])
        self.pack(); self.assertEqual(self.tools('nxaot'), [])   # reused while it is unchanged
        self.pack('linux', '--pack-mode', 'max')
        self.assertEqual([c[-2:] for c in self.tools('nxaot')], [['--profile', str(prof / 'main.tsv')], ['--cache', str(self.wdir / 'aot-cache')]])
        self.assertIn(f'main: sample profile {prof / "main.tsv"} (42 samples at 2 functions): those at tier H+, the others at H', self.said)
        self.pack('linux', '--no-profile', '--pack-mode', 'max')   # ignored: main packed again without it
        self.assertEqual([Path(c[2]).name for c in self.tools('nxaot')], ['main.aot.tsv'])
        self.assertEqual([c for c in self.tools('nxaot') if '--profile' in c], [])
        self.assertIn(f'--no-profile: the sample profiles in {prof} are not used', self.said)
        self.assertEqual(made(launch), [('made', 'profile', 'no'), ('made', 'misses', 'yes'), ('made', 'targets', 'no')])   # a repack (no --no-profile) takes the profile: the launcher says so
        with mock.patch.object(nsp, 'AOT_PROFILE', False):   # the setting off: a repack would not take it either
            self.pack('linux', '--pack-mode', 'max'); self.assertEqual(self.tools('nxaot'), [])
        self.assertEqual(made(launch), [('made', 'profile', 'unused'), ('made', 'misses', 'yes'), ('made', 'targets', 'unused')])
        (prof / 'main.tsv').write_text('0x100 many\n')   # one that does not parse: packed without it, said
        self.pack('linux', '--pack-mode', 'max'); self.assertEqual(self.tools('nxaot'), [])
        self.assertTrue(any(m.startswith(f'[RED] the sample profile {prof / "main.tsv"} does not parse') for m in self.said), self.said)
        self.assertEqual(made(launch), [('made', 'profile', 'yes'), ('made', 'misses', 'yes'), ('made', 'targets', 'yes')])   # packed without it: no row of main
        # an offset repeated (nxaot adds the counts up) and one with no samples (no function hot): the same hot set, so the same digest
        (prof / 'main.tsv').write_text(head + f'# module main, build ID {BID}, image SHA-256 {sha}\n0x100 40\n0x200 2\n0x100 1\n0x300 0\n')
        self.pack('linux', '--pack-mode', 'max')
        self.assertEqual(made(launch), [('made', 'profile', 'yes'), ('made', 'misses', 'yes'), ('made', 'targets', 'yes'), ('samples', 'main', f'2 43 {DIGEST_OFFSETS}')])
        # the host's function lines (dm/host/aot_profile.c): a row per sampled instruction under `# function 0x<entry> <samples> <rows>`; the samples row counts the functions
        # (nxaot's tier H) and their samples (the lines', host calls' too), so a longer session over the same functions gives the same count and digest as the entry form above
        fhead = head + f'# module main, build ID {BID}, image SHA-256 {sha}\n# functions: each sampled function\'s rows follow its line `# function 0x<entry> <samples> <rows>`\n'
        (prof / 'main.tsv').write_text(fhead + '# function 0x100 40 2\n0x104 25\n0x108 13\n# function 0x200 3 1\n0x200 3\n')
        self.pack('linux', '--pack-mode', 'max')
        self.assertIn(f'main: sample profile {prof / "main.tsv"} (43 samples at 2 functions): those at tier H+, the others at H', self.said)
        self.assertEqual(made(launch), [('made', 'profile', 'yes'), ('made', 'misses', 'yes'), ('made', 'targets', 'yes'), ('samples', 'main', f'2 43 {DIGEST_OFFSETS}')])
        (prof / 'main.tsv').write_text(fhead + '# function 0x100 400 5\n' + ''.join(f'0x{o:x} 79\n' for o in range(0x104, 0x118, 4)) + '# function 0x200 30 1\n0x200 30\n')
        self.pack('linux', '--pack-mode', 'max')
        self.assertEqual(made(launch), [('made', 'profile', 'yes'), ('made', 'misses', 'yes'), ('made', 'targets', 'yes'), ('samples', 'main', f'2 430 {DIGEST_OFFSETS}')])   # more rows, the same functions
        self.assertEqual(nsp.profile_summary(['0x104 25', '0x108 13', '0x200 3']), (3, 41, nsp.fnv64('104\n108\n200\n')))   # without function lines: the rows, as before
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
        self.assertEqual(made(self.work / 'launch-linux.tsv'), [('made', 'profile', 'yes'), ('made', 'misses', 'yes'), ('made', 'targets', 'yes'), ('misses', 'main', f'2 {DIGEST_MISSES}')])
        misses('main', sha, [(0x208, 'indirect', 9), (0x100, 'nonlocal', 4)])   # a later session: the same addresses, other counts
        self.pack(); self.assertEqual(self.tools('nxaot'), [])                      # the counts are in no key
        misses('main', sha, [(0x208, 'indirect', 9), (0x100, 'nonlocal', 4), (0x300, 'host', 1)])   # a new address: packed again
        self.pack(); self.assertEqual([c[-2:] for c in self.tools('nxaot')], [['--misses', str(prof / 'main.misses.tsv')]])
        self.assertEqual(made(self.work / 'launch-linux.tsv')[3:], [('misses', 'main', '3 ' + nsp.fnv64('100 nonlocal\n208 indirect\n300 host\n'))])
        self.pack('linux', '--no-profile'); self.assertEqual(self.tools('nxaot'), [])   # --no-profile leaves the misses in use: the same key
        (prof / 'main.tsv').write_text(head.replace('misses', 'profile') + f'# module main, build ID {BID}, image SHA-256 {sha}\n0x100 40\n')   # beside a profile: both
        self.pack(); self.assertEqual([c[-4:] for c in self.tools('nxaot')], [['--profile', str(prof / 'main.tsv'), '--misses', str(prof / 'main.misses.tsv')]])
        (prof / 'main.misses.tsv').write_text('0x208 indirect many\n')   # one that does not parse: packed without it, said
        self.pack(); self.assertEqual([c[-2:] for c in self.tools('nxaot')], [['--profile', str(prof / 'main.tsv')]])
        self.assertTrue(any(m.startswith(f'[RED] the JIT misses {prof / "main.misses.tsv"} do not parse') for m in self.said), self.said)

    def test_known_targets(self):   # <NSP folder>/<id>-work/profile/<module>.targets.tsv (dm/host/aot_targets.c): nxaot reads it beside --profile; its dominant targets in the key and the made rows
        # the rule on the fixture the launcher's test reads too: nxaot's 90 %, the digest of the pairs
        rows = [f'{s:#x} {t:#x} {n}' for s, t, n in TARGET_ROWS]
        self.assertEqual(nsp.targets_summary(rows), DOMINANT)
        self.assertEqual(nsp.targets_digest(DOMINANT), DIGEST_TARGETS); self.assertEqual(DIGEST_TARGETS, nsp.fnv64('1a64:1820\n2000:3000\n2300:3300\n'))
        self.assertEqual(nsp.targets_summary(rows + ['0x1a64 0x1820 7', '1A64\t0X1820 2 # a comment', '# a line']), DOMINANT)   # more binds at the same targets: the same pairs
        self.assertEqual(nsp.targets_summary(['0x10 0x20 1', '0x10 0x30 1']), ())                                              # 50/50: no dominant target
        self.assertEqual(nsp.targets_summary(['0x10 0x30 9', '0x10 0x20 9']), ())                                              # a tie never reaches 90 %
        with self.assertRaises(ValueError): nsp.targets_summary(['0x10 0x20'])                                                 # nxaot stops on it
        self.pack()
        sha = (self.aot / 'main.aot.tsv').read_text().split('\n', 1)[0].split('\t')[3]
        launch, prof, tmp = self.work / 'launch-linux.tsv', self.wdir / 'profile', self.work / 'tmp/aot'
        prof.mkdir(parents=True)
        head = ('# nxaot targets: what the x86-64 engine\'s inline caches of indirect calls and branches were bound to (dm/host/aot_targets.c, DM_AOT_PROFILE); '
                'nxaot build --profile <module>.tsv reads it beside the profile\n')
        def targets(image, rows, name='main'):   # the host's form
            (prof / f'{name}.targets.tsv').write_text(head + f'# module {name}, build ID {BID}, image SHA-256 {image}\n# pack x\n# <site module offset> <target module offset> <binds>\n'
                                                      + ''.join(f'{s:#x} {t:#x} {n}\n' for s, t, n in rows))
        targets(sha, TARGET_ROWS)
        self.pack(); self.assertEqual(self.tools('nxaot'), [])   # no profile: nxaot would not read them, so neither the key nor the made rows take them
        self.assertEqual(made(launch), [('made', 'profile', 'yes'), ('made', 'misses', 'yes'), ('made', 'targets', 'yes')])
        (prof / 'main.tsv').write_text(f'# nxaot profile\n# module main, build ID {BID}, image SHA-256 {sha}\n0x100 40\n0x200 2\n')
        self.pack()
        self.assertEqual(self.tools('nxaot'), [[str(self.nxaot), 'build', str(self.aot / 'main.aot.tsv'), '-o', str(tmp / 'main.o'), '--shared', str(tmp / 'main.aot.so'), '--mode', 'balanced',
                                                '--cache', str(self.wdir / 'aot-cache'), '--profile', str(prof / 'main.tsv')]])
        self.assertEqual(self.targets_switch, [None])   # nxaot reads the file beside the profile itself
        self.assertIn(f'main: sample profile {prof / "main.tsv"} (42 samples at 2 functions) and 3 known targets (main.targets.tsv: indirect call sites with a dominant target, a direct '
                      'call at those in functions at tier H): those at tier H, the others at C', self.said)
        dominant = [('made', 'profile', 'yes'), ('made', 'misses', 'yes'), ('made', 'targets', 'yes'), ('samples', 'main', f'2 42 {DIGEST_OFFSETS}'), ('targets', 'main', f'3 {DIGEST_TARGETS}')]
        self.assertEqual(made(launch), dominant)
        key = (self.aot / '.keys/main.key').read_text()
        targets(sha, TARGET_ROWS + [(0x1a64, 0x1820, 7), (0x2000, 0x3000, 9)])   # a later session: more binds at the same dominant targets, the same pack
        self.pack(); self.assertEqual(self.tools('nxaot'), []); self.assertEqual(made(launch), dominant)
        targets(sha, TARGET_ROWS + [(0x2400, 0x3400, 1)])   # a new site with a dominant target: packed again, and the key holds it
        self.pack(); self.assertEqual([c[-2:] for c in self.tools('nxaot')], [['--profile', str(prof / 'main.tsv')]])
        self.assertNotEqual((self.aot / '.keys/main.key').read_text(), key)
        self.assertEqual(made(launch)[-1], ('targets', 'main', '4 ' + nsp.fnv64('1a64:1820\n2000:3000\n2300:3300\n2400:3400\n')))
        targets(sha, TARGET_ROWS + [(0x2000, 0x3100, 16)])   # 0x2000 now 18 and 18: no dominant target, its guard goes: packed again
        self.pack(); self.assertEqual(len(self.tools('nxaot')), 1)
        self.assertEqual(made(launch)[-1], ('targets', 'main', '2 ' + nsp.fnv64('1a64:1820\n2300:3300\n')))
        targets('00' * 32, TARGET_ROWS)   # recorded on another image: not taken, and kept from nxaot
        self.pack(); self.assertEqual((len(self.tools('nxaot')), self.targets_switch), (1, ['0']))
        self.assertTrue(any(m.startswith(f'the known targets {prof / "main.targets.tsv"} were recorded on another image of main') for m in self.said), self.said)
        self.assertEqual(made(launch), dominant[:-1])
        targets(sha, TARGET_ROWS); self.pack(); self.assertEqual(made(launch), dominant)   # this image's again: the first key
        self.assertEqual((self.aot / '.keys/main.key').read_text(), key)
        (prof / 'main.targets.tsv').write_text(f'# module main, build ID {BID}, image SHA-256 {sha}\n0x2000 0x3000 many\n')   # a row nxaot would stop on: not taken, kept from it
        self.pack(); self.assertEqual(self.targets_switch, ['0'])
        self.assertTrue(any(m.startswith(f'[RED] the known targets {prof / "main.targets.tsv"} do not parse') for m in self.said), self.said)
        self.assertEqual(made(launch), dominant[:-1])
        targets(sha, [(0x10, 0x20, 1), (0x10, 0x30, 1)])   # taken, but no site with a dominant target: the pack of the profile alone, its key the one without the file
        self.pack(); self.assertEqual(made(launch)[-1], ('targets', 'main', f'0 {nsp.fnv64("")}'))
        targets(sha, TARGET_ROWS); self.pack()
        self.pack('linux', '--no-profile')   # without the profile nxaot reads no targets: made targets no, as made profile
        self.assertEqual([c for c in self.tools('nxaot') if '--profile' in c], [])
        self.assertEqual(made(launch), [('made', 'profile', 'no'), ('made', 'misses', 'yes'), ('made', 'targets', 'no')])
        with mock.patch.dict(os.environ, {'NXAOT_PROFILE_TARGETS': '0'}):   # nxaot's test switch, passed on to it: no targets taken, a repack with it would take none either
            self.pack()
        self.assertEqual(made(launch), [('made', 'profile', 'yes'), ('made', 'misses', 'yes'), ('made', 'targets', 'unused'), ('samples', 'main', f'2 42 {DIGEST_OFFSETS}')])
        self.assertNotIn('known targets', ' '.join(self.said))
        self.pack(); self.assertEqual(made(launch), dominant)
        self.assertEqual((self.aot / '.keys/main.key').read_text(), key)

    def test_loader_bindings(self):   # <NSP folder>/<id>-work/profile/<module>.bind.tsv (dm/host aot_profile.c): nxaot build --bind, its pairs in the key, the file beside the pack, take-overs left out
        self.pack()
        sha = (self.aot / 'main.aot.tsv').read_text().split('\n', 1)[0].split('\t')[3]
        prof = self.wdir / 'profile'; prof.mkdir(parents=True)
        head = f'# nxaot bindings: x\n# module main, build ID {BID}, image SHA-256 {sha}\n# <slot> <target> <symbol>\n'
        (prof / 'main.bind.tsv').write_text(head + '0x10 0x200 nn_keep\n0x18 0x240 nn_taken\n')
        tmp, beside = self.work / 'tmp/aot', self.aot / 'main.bind.tsv'
        with mock.patch.object(nsp, 'host_takeovers', return_value=None):   # a host that cannot be asked: the file as recorded
            self.pack()
        self.assertEqual([c[-2:] for c in self.tools('nxaot')], [['--bind', str(prof / 'main.bind.tsv')]])
        self.assertIn(f'main: loader bindings {prof / "main.bind.tsv"} (2 PLT slots bound to its own functions): its stubs are tail calls of them in its pack', self.said)
        self.assertTrue(any('cannot be asked for its take-overs here' in m for m in self.said), self.said)
        self.assertEqual(beside.read_text(), (prof / 'main.bind.tsv').read_text())   # beside the pack: what the launcher reads (launcher_model.h bindings_problem)
        key = (self.aot / '.keys/main.key').read_text()
        with mock.patch.object(nsp, 'host_takeovers', return_value={'nn_other'}):   # a host that takes over none of its names: the same pack
            self.pack()
        self.assertEqual(self.tools('nxaot'), [])
        # a bundle update whose host takes nn_taken over (the file came from the older host): the pack is made again without that row, and the file beside it says so
        with mock.patch.object(nsp, 'host_takeovers', return_value={'nn_taken', 'nn_other'}):
            self.pack()
        self.assertEqual([c[-2:] for c in self.tools('nxaot')], [['--bind', str(tmp / 'main.bind.tsv')]])
        self.assertTrue(any(m.startswith(f'the loader bindings {prof / "main.bind.tsv"} bind 1 name the host takes over (nn_taken)') for m in self.said), self.said)
        self.assertEqual(beside.read_text(), head + '0x10 0x200 nn_keep\n')
        self.assertNotEqual((self.aot / '.keys/main.key').read_text(), key)
        self.assertEqual([n for k, n, v in launch_rows(self.work / 'launch-linux.tsv') if k == 'pack'], ['balanced'])   # complete with the key packs_ready gives too
        with mock.patch.object(nsp, 'host_takeovers', return_value={'nn_taken'}):
            self.pack()
        self.assertEqual(self.tools('nxaot'), [])   # the same pairs: reused
        (prof / 'main.bind.tsv').write_text(head.replace(sha, '00' * 32) + '0x10 0x200 nn_keep\n')   # recorded on another image: packed without, the file beside it gone
        self.pack()
        self.assertEqual([c[-2:] for c in self.tools('nxaot')], [['--cache', str(self.wdir / 'aot-cache')]])
        self.assertFalse(beside.exists())
        # the host asked for real: nxhost-run --takeovers, its own log lines before the list, on this system only
        host = self.tmp / 'hostbin/nxhost-run'; host.parent.mkdir()
        host.write_text(f'#!{sys.executable}\nimport sys\nassert sys.argv[1:] == ["--takeovers"]\nprint("[dm] a constructor line")\nprint("# nxhost-run take-overs: 2 names")\n'
                        'print("nn_a")\nprint("nn_b")\n')
        host.chmod(0o755)
        if sys.platform.startswith('linux'):
            self.assertEqual(nsp.host_takeovers(host, 'linux'), {'nn_a', 'nn_b'})
        self.assertIsNone(nsp.host_takeovers(host, 'windows' if os.name != 'nt' else 'linux'))   # another system's host is not run
        self.assertIsNone(nsp.host_takeovers(self.tmp / 'hostbin/none', 'linux'))
        host.write_text(f'#!{sys.executable}\nprint("usage: nxhost-run ...")\n')   # an older host: no list
        self.assertIsNone(nsp.host_takeovers(host, 'linux'))
        # nxhost-run --bindings: the host writes the module's bindings into the profile folder before the pack, which takes them (loader_bindings)
        host.write_text(f'#!{sys.executable}\nimport sys, os\na = sys.argv[1:]\nassert a[0] == "--profile" and a[2] == "--bindings", a\n'
                        f'open(os.path.join(a[3], "main.bind.tsv"), "w").write({head!r} + "0x20 0x300 nn_dry\\n")\n')
        logs = self.tmp / 'logs'
        self.assertTrue(nsp.loader_bindings(host, self.work / 'game-profile-linux.tsv', self.aot, prof, logs))
        self.assertEqual((prof / 'main.bind.tsv').read_text(), head + '0x20 0x300 nn_dry\n')
        self.assertTrue(any(m.startswith(f'loader bindings: {host} --bindings wrote') for m in self.said), self.said)
        self.assertFalse(nsp.loader_bindings(self.tmp / 'hostbin/none', self.work / 'game-profile-linux.tsv', self.aot, prof, logs))   # no host here: nothing
        with mock.patch.object(nsp, 'host_takeovers', return_value={'nn_taken'}), mock.patch.object(nsp, 'port_host', return_value=(host, True)):
            self.pack()   # the pack: the host's bindings first, then nxaot with them
        self.assertEqual([c[-2:] for c in self.tools('nxaot')], [['--bind', str(prof / 'main.bind.tsv')]])

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
        self.assertEqual(made(self.work / 'launch-linux.tsv'), [('made', 'profile', 'unused'), ('made', 'misses', 'unused'), ('made', 'targets', 'unused')])   # a repack with it would take neither
        self.assertIn('this nxaot has no --cache option: every function is compiled again on each pack', self.said)
        self.assertEqual(nsp.nxaot_options(self.nxaot, dict(os.environ)), {'--cache', '--workers', '--target', '--profile', '--misses', '--bind'})
        (self.aot / 'main.aot.so').unlink(); self.pack('linux', '--aot-workers', '3')
        self.assertEqual(self.tools('nxaot')[0][-4:], ['--cache', str(self.wdir / 'aot-cache'), '--workers', '3'])

    def test_nxaot_missing_or_broken(self):
        repo = self.tmp / 'repo'
        with self.assertRaisesRegex(nsp.BuildError, r'nxaot is not built .*docker build -t nxnative-aot .*--target nxaot'): nsp.find_nxaot(repo)
        (repo / 'build/aot').mkdir(parents=True); (repo / 'build/aot/nxaot').write_text('')
        self.assertEqual(nsp.find_nxaot(repo), repo / 'build/aot/nxaot')
        # on Windows: the cross-build of dm/tools/aot/README.md ("Building nxaot for Windows"), build/aot-win64/nxaot.exe, never the Linux build/aot/nxaot
        with self.assertRaisesRegex(nsp.BuildError, r'nxaot.exe is not built .*aot-win64.*Building nxaot for Windows'): nsp.find_nxaot(repo, win=True)
        (repo / 'build/aot-win64').mkdir(parents=True); (repo / 'build/aot-win64/nxaot.exe').write_text('')
        self.assertEqual(nsp.find_nxaot(repo, win=True), repo / 'build/aot-win64/nxaot.exe')
        (repo / 'bin').mkdir(); (repo / 'bin/nxaot').write_text('')   # the port's bundle (scripts/package_port.py): its bin/nxaot first
        self.assertEqual(nsp.find_nxaot(repo), repo / 'bin/nxaot')
        (repo / 'bin/nxaot.exe').write_text(''); self.assertEqual(nsp.find_nxaot(repo, win=True), repo / 'bin/nxaot.exe')
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

    def test_vulkan_tools_not_matching_the_lock(self):   # dmsh_chain_vk.py --check-tools fails (a SPIR-V tool missing or of another version): not run, its reasons in the summary
        self.vk_tools_bad = True; self.pack()
        self.assertEqual(self.tools('dmsh_build_vk.py'), [])
        self.assertEqual([c[2:] for c in self.tools('dmsh_chain_vk.py') if '--check-tools' in c], [['--check-tools']])
        self.assertIn('[RED] not run on this system: the NVN shaders for Vulkan (the tools here do not match dm/gfx/tools/dmsh_chain_vk.lock: spirv-cross_linux-x64: lock '
                      'vulkan-sdk-1.4.363.0 / actual missing)', self.said)
        self.assertTrue((self.aot / 'main.aot.so').is_file())

    def test_without_a_letter_font(self):   # no letter font on this system (or no fontTools): the font step is not run, no DM_FONT_DIR row; the rest is packed
        self.letters.unlink(); self.pack()
        self.assertEqual(self.tools('make_ext_font.py'), [])
        self.assertNotIn('DM_FONT_DIR', [n for k, n, v in launch_rows(self.work / 'launch-linux.tsv') if k == 'env'])
        self.assertTrue(any(m.startswith('[RED] not run on this system: ') and 'the button-icon font (no letter font on this system (make_ext_font.py looks for ' in m
                            for m in self.said), self.said)
        self.assertTrue((self.aot / 'main.aot.so').is_file())
        with mock.patch.object(nsp, '_importable', side_effect=lambda m: m != 'fontTools'): self.letters.write_bytes(b'letters'); self.pack()
        self.assertEqual(self.tools('make_ext_font.py'), [])
        self.assertTrue(any('the button-icon font (the current Python (' in m and ') has no fontTools (pip install fonttools))' in m for m in self.said), self.said)
        self.pack(); self.assertEqual(len(self.tools('make_ext_font.py')), 1)   # both there: the font is made, and named
        self.assertIn(('env', 'DM_FONT_DIR', str(self.work / 'font')), launch_rows(self.work / 'launch-linux.tsv'))

    def test_bundle_spirv_tools_on_path(self):   # packed from a bundle with the SPIR-V tools (scripts/package_port.py --spirv): its bin/ first on PATH, for this process and its tools
        bundle = self.tmp / 'bundle'
        for rel in nsp.BUNDLE_SPIRV: (bundle / rel).parent.mkdir(parents=True, exist_ok=True); (bundle / rel).write_text('')
        self.assertEqual(nsp.bundled_spirv(bundle), bundle / 'bin')
        (bundle / 'bin/spirv-cross').unlink(); self.assertIsNone(nsp.bundled_spirv(bundle))   # all four, or none
        (bundle / 'bin/spirv-cross').write_text('')
        seen = []
        with mock.patch.object(nsp, 'bundled_spirv', return_value=bundle / 'bin'), mock.patch.object(nsp, 'port_font', side_effect=lambda *a: seen.append(os.environ['PATH'])):
            self.pack()
        self.assertEqual(self.tool_env['dmsh_chain_vk.py']['PATH'].split(os.pathsep)[0], str(bundle / 'bin'))
        self.assertEqual(self.tool_env['dmsh_build_vk.py']['PATH'].split(os.pathsep)[0], str(bundle / 'bin'))
        self.assertEqual(seen[0].split(os.pathsep)[0], str(bundle / 'bin'))   # this process's own PATH too, during the run (Windows looks programs up on it)
        self.assertIn(f"SPIR-V tools: the bundle's, in {bundle / 'bin'} (spirv-val, spirv-link, spirv-cross, glslangValidator)", self.said)
        self.pack(); self.assertNotEqual(self.tool_env.get('dmsh_chain_vk.py', {}).get('PATH', '').split(os.pathsep)[0], str(bundle / 'bin'))   # a checkout's run: PATH as it is

    def test_bundle_tools(self):   # packed from the port's bundle (scripts/package_port.py --tools): its published bin/nx_extract and bin/dmsh_xlate_vk, and no dotnet anywhere
        bundle = self.tmp / 'bundle'
        for rel in (nsp.BUNDLE_NX_EXTRACT, nsp.BUNDLE_XLATE_VK): (bundle / rel).parent.mkdir(parents=True, exist_ok=True); (bundle / rel).write_text('')
        self.assertEqual(REAL_BUNDLED_TOOL(bundle, nsp.BUNDLE_XLATE_VK), bundle / 'bin/dmsh_xlate_vk')
        self.assertIsNone(REAL_BUNDLED_TOOL(self.tmp / 'checkout', nsp.BUNDLE_NX_EXTRACT))
        (bundle / 'python').mkdir()   # a bundle with its own Python: a Python without the modules is told about pack.sh
        with mock.patch.object(nsp, '_importable', return_value=False), \
                self.assertRaisesRegex(nsp.BuildError, r"is missing modules: lz4\.block, capstone, backports\.zstd \(this bundle's pack\.sh runs its own Python, python/"):
            nsp.check_prereqs_port({'PATH': ''}, bundle)
        no_dotnet = lambda name, **kw: None if name == 'dotnet' else TRUE
        with mock.patch.object(nsp.shutil, 'which', side_effect=no_dotnet):   # a checkout without dotnet stops before the long steps
            with self.assertRaisesRegex(nsp.BuildError, r"dotnet \(\.NET SDK 10, which builds nx_extract; the port's bundle carries it prebuilt as bin/nx_extract\)"):
                nsp.check_prereqs_port({'PATH': ''}, self.tmp / 'checkout')
        env = {k: v for k, v in os.environ.items() if k != 'XLATE_VK'}
        with mock.patch.object(nsp, 'bundled_tool', side_effect=lambda repo, rel: REAL_BUNDLED_TOOL(bundle, rel)), mock.patch.object(nsp, 'build_nx_extract', REAL_BUILD_NX_EXTRACT), \
                mock.patch.object(nsp.shutil, 'which', side_effect=no_dotnet), mock.patch.dict(os.environ, env, clear=True):
            self.pack()
            self.assertEqual(nsp.extract.call_args.args[0], bundle / 'bin/nx_extract')
        self.assertIn(f"nx_extract: {bundle / 'bin/nx_extract'} (the bundle's, published: no .NET needed)", self.said)
        self.assertEqual(self.tool_env['dmsh_chain_vk.py']['XLATE_VK'], str(bundle / 'bin/dmsh_xlate_vk'))   # --check-tools, then the build, with the bundle's translator
        self.assertEqual(self.tool_env['dmsh_build_vk.py']['XLATE_VK'], str(bundle / 'bin/dmsh_xlate_vk'))
        self.assertEqual(len(self.tools('dmsh_build_vk.py')), 1)
        self.assertFalse([c for t, c in self.ran if 'dotnet' in c])
        self.assertFalse(any(m.startswith('[RED] not run on this system') for m in self.said))

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

    def test_bundle_host(self):   # packed from the port's bundle (scripts/package_port.py): the launch file names its bin/nxhost-run, the one host, for a registered and a derived profile
        bundle, real = self.tmp / 'bundle', nsp.port_host
        host = bundle / 'bin/nxhost-run'; host.parent.mkdir(parents=True); host.write_text('')
        self.assertEqual(real(bundle, 'linux'), (host, True))
        self.assertEqual(real(bundle, 'windows'), (bundle / 'build/win64/nxhost-run.exe', False))   # a Windows bundle's is bin/nxhost-run.exe
        (bundle / 'bin/nxhost-run.exe').write_text(''); self.assertEqual(real(bundle, 'windows'), (bundle / 'bin/nxhost-run.exe', True))
        self.assertEqual(real(self.tmp / 'checkout', 'linux'), (self.tmp / 'checkout/build/linux/nxhost-run', False))   # a checkout: its build tree's, built or not
        self.pack()   # from a checkout, the last line names the launcher's sources
        done = [m for m in self.said if m.startswith('done: ')]
        self.assertEqual(len(done), 1, self.said); self.assertIn(' or in the launcher nxlauncher (dm/launcher) (', done[0])
        with mock.patch.object(nsp, 'port_host', side_effect=lambda repo, platform: real(bundle, platform)):
            self.pack()
            # from a bundle, the bundle's own launcher, by its path: a player's folder has no dm/launcher
            done = [m for m in self.said if m.startswith('done: ')]
            self.assertEqual(len(done), 1, self.said); self.assertIn(f' or in the launcher {bundle / "nxlauncher"} (nxhost-run {host}, ', done[0])
            self.assertNotIn('dm/launcher', done[0])
            self.assertIn(('path', 'host', str(host)), launch_rows(self.work / 'launch-linux.tsv'))
            self.assertIn(f"nxhost-run is the port's bundle's: {host}", (self.work / 'run.sh').read_text())
            self.check_run_sh()
            nso(self.game / 'ExeFS/main', BID2); self.pack()   # a derived profile: the same host, given the derived profile's file
            self.assertIn(('path', 'host', str(host)), launch_rows(self.work / 'launch-linux.tsv'))
            self.assertIn(f"nxhost-run is the port's bundle's: {host}", (self.work / 'run.sh').read_text())
            self.assertIn(f'display_versions\t{BID2}=1.0.0\n', (self.work / 'game-profile-linux.tsv').read_text(encoding='utf-8'))
            self.check_run_sh()

    def test_old_bundle_layout(self):   # run from a bundle of the layout before the one host (hosts/<title ID>/nxhost-run, no bin/nxhost-run): a clear error at the start, no traceback
        old = self.tmp / 'old bundle'
        (old / 'hosts' / ID).mkdir(parents=True); (old / 'hosts' / ID / 'nxhost-run').write_text('')
        with self.assertRaisesRegex(nsp.BuildError, r'first layout \(hosts/<title ID>/nxhost-run, one host per game profile\) and has no bin/nxhost-run, the one host for every '
                                                    r'profile that this packer\'s packs need: unpack a newer bundle'):
            nsp.port_host(old, 'linux')
        with mock.patch.object(nsp, 'find_repo', return_value=old):
            with self.assertRaisesRegex(nsp.BuildError, 'unpack a newer bundle'): self.pack()
        self.assertEqual((self.rewrites, [t for t, _ in self.ran]), ([], []))   # before any step
        self.assertFalse((self.work / 'launch-linux.tsv').exists())
        self.pack(); self.assertIn(('path', 'host', str(REPO / 'build/linux/nxhost-run')), launch_rows(self.work / 'launch-linux.tsv'))   # from the checkout: packed

    def test_moved_bundle_host(self):   # the launch file's host is gone (a bundle moved or replaced): run.sh takes bin/nxhost-run of the first folder above it that is a bundle
        old, real = self.tmp / 'old bundle', nsp.port_host
        with mock.patch.object(nsp, 'port_host', side_effect=lambda repo, platform: real(old, platform)):
            row = old / 'bin/nxhost-run'; row.parent.mkdir(parents=True); row.write_text('')
            self.pack()   # packed from a bundle in 'old bundle', which is then removed
        self.assertIn(('path', 'host', str(row)), launch_rows(self.work / 'launch-linux.tsv'))
        shutil.rmtree(old)
        sh = (self.work / 'run.sh').read_text()
        self.assertIn('a bundle that holds this NSP folder', sh)
        # the NSP folder sits in the new bundle (self.tmp, two folders above run.sh: nxlauncher beside bin/nxhost-run): its host is the one started, and run.sh says so
        own = self.tmp / 'bin/nxhost-run'; own.write_text(FAKE_HOST.format(py=sys.executable)); own.chmod(0o755)
        out = self.tmp / 'host.json'
        def run(**env):
            out.unlink(missing_ok=True)
            r = subprocess.run(['sh', str(self.work / 'run.sh'), 'main'], env={'PATH': os.environ['PATH'], 'FAKE_OUT': str(out), **env}, capture_output=True, text=True, timeout=60)
            return r.returncode, r.stderr, json.loads(out.read_text()) if out.is_file() else None
        missing = (1, f'[RED] run.sh: the host {row} is missing: pack the game again or set NXHOST\n', None)
        self.assertEqual(run(), missing)   # bin/nxhost-run without nxlauncher beside it is no bundle (a ~/bin of one's own): not taken
        (self.tmp / 'nxlauncher').write_text('')
        rc, err, got = run()
        self.assertEqual((rc, err), (0, f"run.sh: host: the bundle's {own} (the launch file's {row} is missing)\n"))
        self.assertEqual((got['self'], got['argv']), (str(own), ['--profile', str(self.work / 'game-profile-linux.tsv'), str(self.aot), 'main']))
        # NXHOST wins over the bundle's
        rc, err, got = self.run_sh('main')
        self.assertEqual((rc, err, got['self']), (0, '', str(self.tmp / 'nxhost/nxhost-run')))
        # the row's host, there again, wins over the bundle's: nothing said
        row.parent.mkdir(parents=True); shutil.copy(own, row)
        rc, err, got = run()
        self.assertEqual((rc, err, got['self']), (0, '', str(row)))
        shutil.rmtree(old)
        # neither: the red line, and nothing started
        own.unlink()
        self.assertEqual(run(), missing)

    def test_derived_profile(self):   # files of another build under the registered tag: packed from a derived profile, whose file the one host takes (no host to configure)
        nso(self.game / 'ExeFS/main', BID2); self.pack()
        # the red line says first what it means for the player and what to do (pack a version the profile was brought up on), then the details
        red = [m for m in self.said if m.startswith('[RED] this linux pack uses v0 ')]
        self.assertEqual(len(red), 1, self.said)
        self.assertIn('was not brought up on (v0 1.0.0), and the game may not run as well: these files are another build of it than the one the profile was brought up on. '
                      'Details: ', red[0])
        self.assertIn('main is a build the profile does not register', red[0])
        pdir = self.work / 'profile'
        self.assertEqual(game.Game(pdir / ID).builds()['v0']['main'], BID2)
        sh = (self.work / 'run.sh').read_text()
        self.assertNotIn('DM_GAMES_DIR', sh); self.assertIn('-DGAME=<any title ID of dm/games>', sh)
        self.assertIn(('env', 'DM_NVN_SHMAP', str(pdir / ID / 'shader_map.tsv')), launch_rows(self.work / 'launch-linux.tsv'))
        # the profile file: the derived profile's (DM_GAMES_DIR and DM_GAMES_FIRST in game.py's environment), not the registered one's
        gprof = (self.work / 'game-profile-linux.tsv').read_text(encoding='utf-8')
        self.assertEqual(self.tool_env['game.py'].get('DM_GAMES_FIRST'), '1'); self.assertTrue(self.tool_env['game.py']['DM_GAMES_DIR'].startswith(str(pdir)))
        self.assertIn(f'from {pdir / ID}/game.tsv', gprof.splitlines()[0]); self.assertIn(f'display_versions\t{BID2}=1.0.0\n', gprof)
        self.assertEqual(self.check_run_sh()['argv'][:2], ['--profile', str(self.work / 'game-profile-linux.tsv')])
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
        self.assertIn(('path', 'game_profile', str(self.work / 'game-profile-windows.tsv')), rows)   # its own file beside the Linux pack's (another version may be packed for each)
        self.assertEqual((self.work / 'game-profile-windows.tsv').read_text(encoding='utf-8'), game.runtime(game.load(ID)))
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
        # the launch file's host gone and no NXHOST: bin\nxhost-run.exe of the first folder above run.cmd that is a bundle (nxlauncher.exe beside it), as run.sh walks up (cmd.exe
        # is not run here)
        self.assertIn('\r\nif defined NXHOST goto nxrun_host_ok\r\nif exist "%NXRUN_host%" goto nxrun_host_ok\r\nset "NXRUN_DIR=%~dp0"\r\n:nxrun_up\r\n'
                      'if exist "%NXRUN_DIR%bin\\nxhost-run.exe" if exist "%NXRUN_DIR%nxlauncher.exe" goto nxrun_bundle_host\r\n', cmd)
        self.assertIn('\r\nset "NXRUN_host=%NXRUN_DIR%bin\\nxhost-run.exe"\r\n:nxrun_host_ok\r\n', cmd)
        self.assertNotIn('NXRUN_ID', cmd); self.assertNotIn('hosts\\', cmd)
        self.assertIn('\r\necho [RED] run.cmd: the host "%NXRUN_host%" is missing: pack the game again or set NXHOST 1>&2\r\ngoto nxrun_end\r\n', cmd)
        # the game profile's file: the path row (cleared first, as the other path rows), its absence and a file that is gone each a red line, then --profile on the command line
        self.assertIn('\r\nset "NXRUN_game_profile="\r\n', cmd)
        self.assertIn('\r\nif not defined NXRUN_game_profile (\r\n    echo [RED] run.cmd: "%NXRUN_LAUNCH%" names no game profile: pack the game again with '
                      'scripts/build_ipa_from_nsp.py --platform windows 1>&2\r\n    goto nxrun_end\r\n)\r\n', cmd)
        self.assertIn('\r\n:nxrun_host_ok\r\nif not exist "%NXRUN_game_profile%" (\r\n    echo [RED] run.cmd: the game profile "%NXRUN_game_profile%" is missing: pack the game again '
                      '1>&2\r\n    goto nxrun_end\r\n)\r\n', cmd)
        self.assertLess(cmd.index(':nxrun_host_ok\r\n'), cmd.index('\r\n"%NXRUN_host%" --profile "%NXRUN_game_profile%" "%NXRUN_AOT%" %*\r\n'))
        self.assertIn('\r\n"%NXRUN_host%" --profile "%NXRUN_game_profile%" "%NXRUN_AOT%" %*\r\nset "NXRUN_RC=%errorlevel%"\r\n:nxrun_end\r\nif defined NXRUN_CP chcp %NXRUN_CP% > nul\r\n'
                      'exit /b %NXRUN_RC%\r\n', cmd)
        self.assertIn('x86_64-w64-mingw32.cmake', cmd)
        self.assertNotRegex(cmd, r'\([^)\r\n]*\r\n[^)]*echo [^\r\n]*\([^\r\n]*\)')   # no parenthesis inside an echo of a parenthesized block (it would close the block)
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


class ProfileStatus(unittest.TestCase):   # report_profile_status's red line for a version the profile was not brought up on: what to do first, then the details
    def test_unregistered_version(self):   # a player's v0 when the profile was brought up on the update v65536 (Mario vs. Donkey Kong from its .xci alone)
        prof = lambda builds: types.SimpleNamespace(id=ID, title_id=TID, builds=lambda: builds)
        reg, g = prof({'v65536': {'display': '1.0.1', 'main': BID}}), prof({'v0': {'display': '1.0.0', 'main': BID2}})
        said = []
        with mock.patch.object(nsp, 'say', said.append): nsp.report_profile_status(g, reg, 'v0', 'v0 is not a version the profile registers', False, 'linux pack')
        self.assertEqual(len(said), 1, said)
        self.assertTrue(said[0].startswith(f'[RED] this linux pack uses v0 (1.0.0), which game profile {ID} was not brought up on (v65536 1.0.1), and the game may not run as '
                                           'well: to pack a version it was brought up on, put that version\'s NSP (its update) in the NSP folder beside the base game and pack '
                                           'again (-p <version> picks it when a newer update is there too). Details: '), said[0])
        self.assertIn('main is a build the profile does not register', said[0])


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
