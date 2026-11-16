"""Shaders a game compiles with its own GLSLC while it is played, past the boot the NSP script's capture runs (Catherine: Full Body compiles one whenever its shader cache has no file
for a material): the host saves an output with a stage whose shader key its shader map lacks to glslc_play/ beside the save (hle_glslc.c, nvn_shkey.h), the NSP script takes such folders
into <version dir>/glslc_play (gather_glslc_play), and shader_split.py gives each new key a /glslc_play/ row. The keys decide everywhere, not the file names: a game that compiles combinations
of stages its files hold already (Portal, Portal 2) saves nothing and gets no red line. Synthetic GLSLC outputs only, no game files."""
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'scripts')); sys.path.insert(0, str(REPO / 'dm/games'))
import build_ipa_from_nsp as nsp   # noqa: E402
import game   # noqa: E402

GAME = '01007BB017812000'
RED_LINE = '[RED] glslc: the game compiled a shader that this build\'s shader map does not have'


def stage(marker, sph):
    """One control section and its code area: the code's SPH word 0 says the stage (1 << 10 | 1 vertex, 5 << 10 pixel), marker makes the shader its own"""
    ctl, code = bytearray(0x880), bytearray(0x100)
    ctl[:4] = bytes.fromhex('34127698')
    struct.pack_into('<I', ctl, 0x20, len(ctl))
    struct.pack_into('<I', ctl, 0x704, len(code))
    ctl[0x7d0] = marker >> 8   # in GLSLC's compile information: what a profile's shader_key_skip may leave out
    code[:4] = bytes.fromhex('78563412')
    struct.pack_into('<I', code, 0x30, sph)
    code[0x80] = marker & 0xff
    return bytes(ctl), bytes(code)


def whole(data):
    data[:4] = bytes.fromhex('91688619')
    struct.pack_into('<I', data, 0x44, len(data))
    return data


def glslc_output(marker, variant=0):
    """One whole single-stage GLSLC output (magic 0x19866891, its own length at +0x44, a control section with the code area after it). variant changes a header byte only: another file, the same shader key"""
    ctl, code = stage(marker, 1 << 10 | 1)
    data = whole(bytearray(0x70) + ctl + code)
    data[0x08] = variant
    return bytes(data)


def glslc_program(vs, ps):
    """One whole GLSLC output of two stages compiled as one program (a vertex and a pixel shader), read by its section headers: output +0x4c = section count, section i's header at
    +0x90 + 0x90 i = {size, data offset, type 0 = GPU code}, +0x30 control offset, +0x34 code offset, +0x38 code length, +0x3c control length"""
    secs = [b''.join(stage(vs, 1 << 10 | 1)), b''.join(stage(ps, 5 << 10))]
    data = bytearray(0x90 + 0x90 * len(secs))
    struct.pack_into('<I', data, 0x4c, len(secs))
    for i, s in enumerate(secs):
        struct.pack_into('<3I', data, 0x90 + 0x90 * i, len(s), len(data), 0)
        struct.pack_into('<4I', data, 0x90 + 0x90 * i + 0x30, 0, 0x880, len(s) - 0x880, 0x880)
        data += s
    return bytes(whole(data))


def fnv_name(b): return f'{nsp.fnv1a64(b):016x}.glslc'


def split_keys(paths, profile=GAME):   # {file name: [key of each stage]} as shader_split.py reads and keys them
    r = subprocess.run([sys.executable, REPO / 'dm/gfx/tools/shader_split.py', '--glslc-keys', *paths], env=dict(os.environ, DM_GAME=str(profile)), capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    out = {}
    for ln in r.stdout.splitlines():
        n, _, k = ln.split('\t'); out.setdefault(n, []); k != '-' and out[n].append(k)
    return out


def rows(text):   # {source folder and file: key} of a shader map's data rows
    return {c[4].split('#')[0]: c[3] for c in (l.split('\t') for l in text.splitlines() if l and not l.startswith(('#', 'package\t')))}


def split(root, play=None):   # the split of a RomFS-less game with a boot capture and a play folder: its shader map
    out = root / 'out'; shutil.rmtree(out, ignore_errors=True); out.mkdir()
    env = dict(os.environ, DM_GAME=GAME, J='1')
    for name in ('DM_GLSLC_CAPTURE', 'DM_GLSLC_PLAY', 'DM_UE_SHADERS', 'DM_GAMES_DIR'): env.pop(name, None)
    if play: env['DM_GLSLC_PLAY'] = str(play)
    paths = [out / n for n in ('pkg', 'map.tsv', 'facts.tsv', 'pairs.tsv', 'pso.tsv')]
    r = subprocess.run([sys.executable, REPO / 'dm/gfx/tools/shader_split.py', root / 'v0/RomFS', root / 'main', *paths], env=env, capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return (out / 'map.tsv').read_text(), r.stdout


def game_root(t):
    root = Path(t); (root / 'v0/RomFS').mkdir(parents=True); (root / 'main').write_bytes(b'NSO0' + bytes(0xfc))
    return root


class SplitReadsPlay(unittest.TestCase):
    def test_play_captures_get_rows_of_their_own(self):
        with tempfile.TemporaryDirectory() as t:
            root = game_root(t)
            boot, again, new = glslc_output(1), glslc_output(1, variant=7), glslc_output(2)
            (root / 'v0/glslc_capture').mkdir(); (root / 'v0/glslc_capture' / fnv_name(boot)).write_bytes(boot)
            without = rows(split(root)[0])
            self.assertEqual(list(without), [f'/glslc_capture/{fnv_name(boot)}'])
            play = root / 'v0/glslc_play'; play.mkdir()
            for b in (again, new): (play / fnv_name(b)).write_bytes(b)
            (play / '._x.glslc').write_bytes(b'\0\5\26\7' + bytes(60))   # AppleDouble metadata is not an input
            got = rows(split(root)[0])
            # the boot's row stays as it is; the new shader gets a /glslc_play/ row, the copy of the boot's shader none
            self.assertEqual(got, {**without, f'/glslc_play/{fnv_name(new)}': got.get(f'/glslc_play/{fnv_name(new)}')})
            self.assertEqual(len(set(got.values())), 2)
            self.assertEqual(rows(split(root, play=play)[0]), got)   # DM_GLSLC_PLAY names the folder as well
            # a file the split reads no shader out of is left out with a red line; the others still count
            bad = bytearray(new); bad[0x70 + 0x880:0x70 + 0x884] = bytes(4)   # no code area header
            bad = bytes(whole(bad)); (play / fnv_name(bad)).write_bytes(bad)
            m, log = split(root)
            self.assertEqual(rows(m), got)
            self.assertIn(f'[RED] /glslc_play/{fnv_name(bad)}: no GLSLC output this split reads', log)

    def test_keys_of_a_program_and_its_stages(self):
        with tempfile.TemporaryDirectory() as t:
            p, a, b = Path(t) / 'p', Path(t) / 'a', Path(t) / 'b'
            p.write_bytes(glslc_program(3, 4)); a.write_bytes(glslc_output(3)); b.write_bytes(b'not one')
            got = split_keys([p, a, b])
            ctl, code = stage(3, 1 << 10 | 1)
            self.assertEqual(got['a'], [hashlib.sha256(ctl + code).hexdigest()[:16]])   # the first 8 bytes of sha256(control + code), the renderer's key
            self.assertEqual(len(got['p']), 2); self.assertIn(got['a'][0], got['p'])   # the program's vertex stage is that shader
            self.assertEqual(got['b'], [])


class GatherPlay(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        self.prof = self.tmp / 'profile' / GAME; self.prof.mkdir(parents=True)
        shutil.copyfile(REPO / 'dm/games' / GAME / 'game.tsv', self.prof / 'game.tsv')   # the split reads the profile's key rules (shader_key_skip)
        self.d = self.tmp / 'extract/v0'; self.d.mkdir(parents=True)
        self.said = []
        p = mock.patch.object(nsp, 'say', side_effect=self.said.append); p.start(); self.addCleanup(p.stop)

    def profile(self, *rows):
        (self.prof / 'shader_map.tsv').write_text(''.join(f'vs_other.bin\t{i}\tvertex\t{k}\t{src}#GLSLC@0x0\n' for i, (k, src) in enumerate(rows)))
        return game.Game(self.prof)

    def test_takes_whole_outputs_named_by_their_hash(self):
        a, b = glslc_output(1), glslc_output(2)
        s1, s2 = self.tmp / 'nsp/glslc_play', self.tmp / 'device'
        s1.mkdir(parents=True); s2.mkdir()
        (s1 / fnv_name(a)).write_bytes(a)
        (s2 / fnv_name(a)).write_bytes(a); (s2 / fnv_name(b)).write_bytes(b)
        (s2 / '0123456789abcdef.glslc').write_bytes(b)   # renamed: not the name hle_glslc.c gives it
        (s2 / fnv_name(glslc_output(3))).write_bytes(glslc_output(3)[:-8])   # cut short
        (s2 / '._x.glslc').write_bytes(b'\0\5\26\7')
        g = self.profile()
        got = nsp.gather_glslc_play(self.d, [s1, s2, self.tmp / 'absent'], g)
        self.assertEqual(got, {fnv_name(a), fnv_name(b)})
        self.assertEqual(sorted(p.name for p in (self.d / 'glslc_play').iterdir()), sorted(got))
        self.assertEqual(sum('[RED]' in s and 'FNV-1a' in s for s in self.said), 2)
        self.assertIn('2 shaders captured in play that the profile\'s tables lack go into this build\'s shader tables', self.said)
        # nothing supplied any more: the folder goes, so the split's inputs follow the sources
        shutil.rmtree(s1); shutil.rmtree(s2)
        self.assertEqual(nsp.gather_glslc_play(self.d, [s1, s2], g), set())
        self.assertFalse((self.d / 'glslc_play').exists())

    def test_keys_decide_not_names(self):
        a, b, same_as_a = glslc_output(1), glslc_output(2), glslc_output(1, variant=9)
        src = self.tmp / 'nsp/glslc_play'; src.mkdir(parents=True)
        (src / fnv_name(same_as_a)).write_bytes(same_as_a); (src / fnv_name(b)).write_bytes(b)
        unread = bytearray(glslc_output(4)); unread[0x70 + 0x880:0x70 + 0x884] = bytes(4); unread = bytes(whole(unread))   # whole and named by its hash, but no shader the split reads
        (src / fnv_name(unread)).write_bytes(unread)
        keys = split_keys([src / fnv_name(same_as_a), src / fnv_name(b)])
        ka, kb = keys[fnv_name(same_as_a)][0], keys[fnv_name(b)][0]
        # the profile was split from a's file, the folder holds another output with a's key: held. b's key the game files have: nothing new
        g = self.profile((ka, f'/glslc_play/{fnv_name(a)}'), (kb, '/data/b.vs'))
        got = nsp.gather_glslc_play(self.d, [src], g)
        self.assertEqual(got, {fnv_name(same_as_a), fnv_name(b)})
        self.assertFalse(any('the profile names' in s for s in self.said), self.said)
        self.assertFalse(any('go into this build' in s for s in self.said), self.said)
        self.assertIn('1 of the files captured in play hold only shaders the game files have already: they add nothing to the tables', self.said)
        self.assertTrue(any(s.startswith('[RED]') and fnv_name(unread) in s and 'reads no shader' in s for s in self.said), self.said)
        self.assertNotIn(fnv_name(unread), [p.name for p in (self.d / 'glslc_play').iterdir()])
        # a profile row whose key no folder holds: the build says those draws stay missing
        self.said.clear(); nsp.gather_glslc_play(self.d, [src], self.profile((ka, f'/glslc_play/{fnv_name(a)}'), ('f' * 16, f'/glslc_play/{"f" * 16}.glslc')))
        self.assertTrue(any(s.startswith('[RED] the profile names 1 shaders') for s in self.said), self.said)

    def test_nothing_captured_skips_the_map_split_unless_the_profile_has_play_rows(self):
        # no capture and no /glslc_play/ row (every game but a GLSLC one): the shader map is not split into a dict, and the folder of an earlier build still goes
        (self.d / 'glslc_play').mkdir(); (self.d / 'glslc_play' / 'old.glslc').write_bytes(b'x')
        g = self.profile(('1' * 16, '/data/a.vs'))
        with mock.patch.object(nsp, 'shader_sources', wraps=nsp.shader_sources) as ss:
            self.assertEqual(nsp.gather_glslc_play(self.d, [self.tmp / 'absent'], g), set())
            ss.assert_not_called()
        self.assertFalse((self.d / 'glslc_play').exists())
        self.assertFalse(any(s.startswith('[RED]') for s in self.said), self.said)
        # a profile with /glslc_play/ rows and nothing captured: the red line that those draws stay missing is still given
        g = self.profile(('1' * 16, '/data/a.vs'), ('f' * 16, f'/glslc_play/{"f" * 16}.glslc'))
        self.assertEqual(nsp.gather_glslc_play(self.d, [self.tmp / 'absent'], g), set())
        self.assertTrue(any(s.startswith('[RED] the profile names 1 shaders') for s in self.said), self.said)

    def test_env_off_values_name_no_folder(self):
        # the values the host reads as "save nothing" (hle_glslc.c play_dir) are not a folder under the current directory
        for v in ('off', '0', ''): self.assertEqual(nsp.glslc_play_env({'DM_GLSLC_PLAY': v}), [], v)
        self.assertEqual(nsp.glslc_play_env({}), [])
        self.assertEqual(nsp.glslc_play_env({'DM_GLSLC_PLAY': '~/play'}), ['~/play'])

    def test_default_sources(self):
        src = nsp.glslc_play_sources(self.tmp / 'nsp', self.tmp / 'work', 0x0100BF00112C0000, ['~/x'])
        self.assertEqual(src[0], Path.home() / 'x')
        self.assertIn(self.tmp / 'nsp/glslc_play', src); self.assertIn(self.tmp / 'work/run/glslc_play', src)
        self.assertEqual(src[-1], Path.home() / 'Library/Application Support/nxnative/0100BF00112C0000/glslc_play')

    def test_boot_capture_wants_only_boot_rows(self):
        (self.prof / 'shader_map.tsv').write_text(f'vs_other.bin\t0\tvertex\t{"1" * 16}\t/glslc_capture/{"a" * 16}.glslc#GLSLC@0x0\n'
                                                  f'vs_other.bin\t1\tvertex\t{"2" * 16}\t/glslc_play/{"b" * 16}.glslc#GLSLC@0x0\n')
        self.assertEqual(nsp.glslc_wanted(game.Game(self.prof)), {'a' * 16 + '.glslc'})

    def test_tables_differing_only_in_play_rows_derive(self):
        tables = self.tmp / 'tables'; tables.mkdir()
        base = 'vs_data.bin\t0\tvertex\tbase\t/data/a.vs#GLSLC@0x0\n'
        played = 'vs_other.bin\t0\tvertex\tplay\t/glslc_play/aaaaaaaaaaaaaaaa.glslc#GLSLC@0x0\n'
        (self.prof / 'shader_map.tsv').write_text(base + played); (self.prof / 'shader_facts.tsv').write_text('')
        g = game.Game(self.prof)
        for name, split_map, want in (('play capture not supplied', base, True),
                                      ('another play capture', base + played.replace('play\t', 'more\t').replace('a' * 16, 'b' * 16), True),
                                      ('the same', base + played, False),
                                      ('a game file differs', base.replace('base', 'other') + played, False)):
            with self.subTest(name):
                (tables / 'shader_map.tsv').write_text(split_map); (tables / 'shader_facts.tsv').write_text('')
                self.assertEqual(nsp.dlc_only_shader_change(g, tables), want)


# hle_glslc.c with a stand-in glslcCompile, and the renderer's map lookup (nvn_mtl.m mtl_shmap_has) as a reader of DM_NVN_SHMAP's key column
HARNESS = r'''
#include "hle_glslc.c"
static uint8_t g_status[16], *g_out; static const uint8_t *g_res[2];
static uint8_t fake_compile(void *obj) { (void)obj; return 1; }
void *guest_sym(const char *name) { return strcmp(name, "glslcCompile") ? NULL : (void *)fake_compile; }
static char (*g_map)[17]; static size_t g_nmap; static int g_map_read;
int mtl_shmap_has(const char *key) {
    if (!g_map_read) {
        g_map_read = 1; FILE *f = getenv("DM_NVN_SHMAP") ? fopen(getenv("DM_NVN_SHMAP"), "r") : NULL; if (!f) return -1;
        char line[4096]; g_map = malloc(1);
        while (fgets(line, sizeof line, f)) {
            char *c = line; for (int i = 0; i < 3 && c; i++) { c = strchr(c, '\t'); if (c) c++; }
            if (!c || strlen(c) < 16) continue;
            g_map = realloc(g_map, (g_nmap + 1) * sizeof *g_map); memcpy(g_map[g_nmap], c, 16); g_map[g_nmap++][16] = 0;
        }
        fclose(f);
    }
    if (!g_map) return -1;
    for (size_t i = 0; i < g_nmap; i++) if (!strcmp(g_map[i], key)) return 1;
    return 0;
}
int main(int argc, char **argv) {   // each argument: a file holding one GLSLC output, compiled in turn
    g_status[0xc] = 1; const uint8_t *const *res = g_res; void *obj = &res;
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
        g_out = malloc((size_t)n); fread(g_out, 1, (size_t)n, f); fclose(f);
        g_res[0] = g_status; g_res[1] = g_out;
        if (!h_glslc_Compile(obj)) return 2;
    }
    return 0;
}
'''


@unittest.skipUnless(shutil.which('clang') and sys.platform == 'darwin', 'needs clang on a Mac (the host builds against CommonCrypto)')
class HostSavesPlay(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        (cls.tmp / 'harness.c').write_text(HARNESS)
        cls.exe, cls.exe_skip = cls.build('plain', 0, 0), cls.build('skip', 0x7d0, 0x18)

    @classmethod
    def build(cls, name, skip_off, skip_len):
        gen = cls.tmp / name / 'gen'; gen.mkdir(parents=True)
        (gen / 'game_profile.h').write_text(f'#define DM_GAME_ID "0100000000000000"\n#define DM_GAME_SHADER_KEY_SKIP_OFF {skip_off:#x}\n#define DM_GAME_SHADER_KEY_SKIP_LEN {skip_len:#x}\n')
        exe = cls.tmp / name / 'harness'
        r = subprocess.run(['clang', '-Wall', '-Werror', '-Wno-unused-function', '-I', REPO / 'dm/host', '-I', gen, '-o', exe, cls.tmp / 'harness.c'], capture_output=True, text=True)
        if r.returncode: raise AssertionError(r.stderr)
        return exe

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp)

    def run_host(self, root, files, exe=None, **env):
        e = {k: v for k, v in os.environ.items() if not k.startswith('DM_')}
        e.update(DM_SAVE=str(root / 'save'), **env)
        r = subprocess.run([exe or self.exe, *files], env=e, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stderr

    def outputs(self, root, *blobs):
        files = []
        for i, b in enumerate(blobs): (root / f'o{i}').write_bytes(b); files.append(root / f'o{i}')
        return files

    def shader_map(self, root, keys, src='/data/shaders/x.vs'):   # keys under game-file sources: no row names the files the host gets
        p = root / 'shader_map.tsv'
        p.write_text(''.join(f'vs_other.bin\t{i}\tvertex\t{k}\t{src}{i}#GLSLC@0x0\n' for i, k in enumerate(keys)))
        return p

    def played(self, root): return sorted(p.name for p in (root / 'glslc_play').iterdir()) if (root / 'glslc_play').exists() else []

    def test_stages_the_map_has_save_nothing(self):   # (a) a run-time combination of stages the game files hold (Portal): no file, no red line, at every run
        with tempfile.TemporaryDirectory() as t:
            root = Path(t); files = self.outputs(root, glslc_program(1, 2), glslc_output(3), glslc_output(1, variant=5))
            keys = {k for ks in split_keys(files).values() for k in ks}
            self.assertEqual(len(keys), 3)
            m = self.shader_map(root, keys)
            for _ in range(2):
                log = self.run_host(root, files + files, DM_NVN_SHMAP=str(m))
                self.assertEqual(self.played(root), []); self.assertNotIn('[RED]', log)

    def test_one_missing_stage_saves_the_output_once(self):   # (b) one stage's key missing: that output saved once, one red line naming the key; (c) seen again: nothing more
        with tempfile.TemporaryDirectory() as t:
            root = Path(t); prog = glslc_program(1, 2)
            files = self.outputs(root, prog, glslc_output(3))
            vs, ps = split_keys(files[:1])['o0']
            m = self.shader_map(root, [vs, split_keys(files[1:])['o1'][0]])
            log = self.run_host(root, files + files + files, DM_NVN_SHMAP=str(m))
            self.assertEqual(self.played(root), [fnv_name(prog)])
            self.assertEqual((root / 'glslc_play' / fnv_name(prog)).read_bytes(), prog)
            self.assertEqual(log.count(RED_LINE), 1); self.assertIn(f'key {ps}, 1 of the output\'s 2 stages', log)
            self.assertNotIn('not in the shader map either', log)
            # the next run: the folder has it already, the build does not yet: the red line again, no second file
            log = self.run_host(root, files, DM_NVN_SHMAP=str(m))
            self.assertEqual(self.played(root), [fnv_name(prog)]); self.assertEqual(log.count(RED_LINE), 1)
            # another output with the same missing stage (other header bytes) in the same run: neither saved nor reported
            shutil.rmtree(root / 'glslc_play')
            other = bytearray(prog); other[0x08] = 3; other = bytes(other)
            log = self.run_host(root, self.outputs(root, prog, other, prog), DM_NVN_SHMAP=str(m))
            self.assertEqual(self.played(root), [fnv_name(prog)]); self.assertEqual(log.count(RED_LINE), 1)
            self.assertNotIn('not in the shader map either', log)
            # a second missing shader in that run: saved, a plain line
            log = self.run_host(root, self.outputs(root, prog, glslc_output(4)), DM_NVN_SHMAP=str(m))
            self.assertEqual(self.played(root), sorted([fnv_name(prog), fnv_name(glslc_output(4))]))
            self.assertEqual(log.count(RED_LINE), 1); self.assertEqual(log.count('is not in the shader map either'), 1)

    def test_a_missing_stage_in_another_combination_is_saved(self):   # the split takes a program's variation pair only from a file it reads (var_pairs, vs_pad)
        with tempfile.TemporaryDirectory() as t:
            root = Path(t); x, y = glslc_program(1, 2), glslc_program(1, 3)
            files = self.outputs(root, x, y, y)
            (vs, ps2), (_, ps3) = split_keys(files[:1])['o0'], split_keys(files[1:2])['o1']
            m = self.shader_map(root, [ps2, ps3])   # the vertex shader both share is missing
            log = self.run_host(root, files, DM_NVN_SHMAP=str(m))
            self.assertEqual(self.played(root), sorted([fnv_name(x), fnv_name(y)]))
            self.assertEqual(log.count(RED_LINE), 1); self.assertEqual(log.count(f'shader {vs} is not in the shader map either'), 1)

    def test_a_build_with_the_saved_files_knows_them(self):   # (d) the split folds the saved files into the tables: the next run saves and reports nothing
        with tempfile.TemporaryDirectory() as t:
            root = game_root(t)
            boot, prog, single = glslc_output(1), glslc_program(1, 2), glslc_output(3)
            (root / 'v0/glslc_capture').mkdir(); (root / 'v0/glslc_capture' / fnv_name(boot)).write_bytes(boot)
            old_map = root / 'old_map.tsv'; old_map.write_text(split(root)[0])
            files = self.outputs(root, boot, prog, single)
            log = self.run_host(root, files, DM_NVN_SHMAP=str(old_map))
            self.assertEqual(self.played(root), sorted([fnv_name(prog), fnv_name(single)]))   # the boot's shader is in the map: only the others
            self.assertEqual(log.count(RED_LINE), 1)
            # the NSP script takes the folder into <version dir>/glslc_play and splits again
            g = game.Game(REPO / 'dm/games' / GAME)
            with mock.patch.object(nsp, 'say'): nsp.gather_glslc_play(root / 'v0', [root / 'glslc_play'], g)
            new_map = root / 'new_map.tsv'; text = split(root)[0]; new_map.write_text(text)
            self.assertEqual(sorted(k for k in rows(text) if k.startswith('/glslc_play/')), sorted(f'/glslc_play/{n}' for n in (fnv_name(prog), fnv_name(single))))
            shutil.rmtree(root / 'glslc_play')
            log = self.run_host(root, files + files, DM_NVN_SHMAP=str(new_map))
            self.assertEqual(self.played(root), []); self.assertNotIn('[RED]', log)

    def test_key_skip_as_the_profile_says(self):   # a profile's shader_key_skip: the host's key leaves the same control bytes out as the split's
        with tempfile.TemporaryDirectory() as t:
            root = Path(t); prof = root / 'profile' / GAME; prof.mkdir(parents=True)
            prof.joinpath('game.tsv').write_text((REPO / 'dm/games' / GAME / 'game.tsv').read_text() + 'shader_key_skip\t0x7d0+0x18\n')
            a, b = glslc_output(0x105), glslc_output(0x205)   # differ only at control +0x7d0
            files = self.outputs(root, a, b)
            keys = split_keys(files, prof)
            self.assertEqual(keys['o0'], keys['o1'])
            m = self.shader_map(root, keys['o0'])
            log = self.run_host(root, files, exe=self.exe_skip, DM_NVN_SHMAP=str(m))
            self.assertEqual(self.played(root), []); self.assertNotIn('[RED]', log)
            self.assertNotEqual(split_keys(files)['o0'], split_keys(files)['o1'])   # without the skip they are two shaders

    def test_no_map_off_and_unreadable(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t); outs = [glslc_output(i) for i in (1, 2, 3)]; files = self.outputs(root, *outs)
            # no map: every output counts as new; DM_GLSLC_PLAY names the folder, off saves none
            other = root / 'other'
            self.run_host(root, files, DM_NVN_SHMAP=str(root / 'none.tsv'), DM_GLSLC_PLAY=str(other))
            self.assertEqual(sorted(p.name for p in other.iterdir()), sorted(fnv_name(b) for b in outs))
            m = self.shader_map(root, [])
            self.assertIn('not saved', self.run_host(root, files, DM_NVN_SHMAP=str(m), DM_GLSLC_PLAY='off'))
            self.assertEqual(self.played(root), [])
            # an output whose stages neither the host nor the split reads: not saved (it would only stop the next split), one red line
            bad = bytearray(outs[0]); bad[0x70 + 0x880:0x70 + 0x884] = bytes(4); bad = bytes(whole(bad))
            log = self.run_host(root, self.outputs(root, bad, bad), DM_NVN_SHMAP=str(m))
            self.assertEqual(self.played(root), []); self.assertEqual(log.count('whose stages this host cannot read'), 1)

    def test_the_capture_run_saves_every_output(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t); b = glslc_output(1); (root / 'o').write_bytes(b)
            shmap = self.shader_map(root, split_keys([root / 'o'])['o'])
            self.run_host(root, [root / 'o'], DM_NVN_SHMAP=str(shmap), DM_GLSLC_CAPTURE=str(root / 'cap'))
            self.assertEqual([p.name for p in (root / 'cap').iterdir()], [fnv_name(b)])
            self.assertFalse((root / 'glslc_play').exists())


if __name__ == '__main__': unittest.main()
