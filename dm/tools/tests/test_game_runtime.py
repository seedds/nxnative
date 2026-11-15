"""The game profile's port form (dm/games/game.py header --runtime, runtime, table: one nxhost-run for every profile, dm/host/game_rt.c) against Apple's form (game.py header <id>,
a host compiled for one profile): for every profile of dm/games each macro of Apple's header and the same constant in the profile file the port's host reads hold the same value,
and the compiled-in table has the profile's row; the runtime header declares a field and a macro for every constant; a profile outside dm/games (DM_GAMES_DIR) is a file the
host can read and a row of its own when it is the default; what the port's file cannot carry (a tab in a value, lists beyond their bounds) is refused. Temporary files only."""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
GAME_PY = REPO / 'dm/games/game.py'
sys.path.insert(0, str(REPO / 'dm/games'))
import game  # noqa: E402

REGISTERED = [game.Game(d) for d in sorted(game.HERE.iterdir()) if (d / 'game.tsv').exists()]


def macros(text):   # Apple's header: {name after DM_GAME_: value text}
    return dict(re.findall(r'^#define DM_GAME_(\w+) (.*)$', text, re.M))


def file_rows(text):   # the port's profile file: {key: value}
    return dict(l.split('\t', 1) for l in text.split('\n') if l and not l.startswith('#'))


def c_str(s):   # a C string literal as game.py writes it, back to its text
    assert s[0] == s[-1] == '"', s
    return re.sub(r'\\(.)', r'\1', s[1:-1])


class Forms(unittest.TestCase):
    def test_every_profile_same_values(self):
        """Apple's macros and the port's file give each registered profile's constants the same values"""
        self.assertGreater(len(REGISTERED), 1)
        for g in REGISTERED:
            with self.subTest(profile=g.id):
                m, f = macros(game.header(g)), file_rows(game.runtime(g))
                self.assertEqual(list(f), [k for k, _ in game.RUNTIME_FIELDS])   # every constant, in order, once
                for k, kind in game.RUNTIME_FIELDS:
                    v = f[k]
                    if kind == 'str': self.assertEqual(v, c_str(m[k.upper()]), k)
                    elif kind == 'u64': self.assertEqual(int(v, 0), int(m[k.upper()].rstrip('ul'), 0), k)
                    elif kind == 'modules':
                        self.assertEqual(v.split(' '), [c_str(s.strip()) for s in m['MODULES'].strip('{}').split(',')], k)
                        self.assertEqual(len(v.split(' ')), int(m['NMOD']))
                    elif kind == 'versions':
                        pairs = re.findall(r'\{("[^"]*"), ("[^"]*")\}', m['DISPLAY_VERSIONS'])
                        self.assertEqual(v, ' '.join(f'{c_str(b)}={c_str(d)}' for b, d in pairs), k)
                    elif kind.startswith('opt'): self.assertEqual(v, m.get(k.upper(), '-'), k)   # Apple's header leaves an unset one undefined
                    else: self.assertEqual(v, m[k.upper()], k)

    def test_runtime_header(self):
        """the port's header: a field, a DM_GAME_FIELDS entry and (but the optional ones) a macro reading the field for every constant; every macro of Apple's form is there"""
        h = game.runtime_header()
        self.assertIn('#define DM_GAME_RUNTIME 1', h)
        apple = set().union(*(macros(game.header(g)) for g in REGISTERED))
        for k, kind in game.RUNTIME_FIELDS:
            self.assertIn(f'    X({k}, {kind.upper()}) \\', h)
            if kind.startswith('opt'): self.assertNotIn(f'#define DM_GAME_{k.upper()} ', h, 'an #ifdef site would take an unset one as set')
            else: self.assertIn(f'#define DM_GAME_{k.upper()} (dm_game.{k})', h)
        defined = set(re.findall(r'^#define DM_GAME_(\w+) \(dm_game\.', h, re.M))
        self.assertLessEqual(apple - defined, {'SHARED_FONT_TTF', 'NVN_API_MAJOR', 'NVN_API_MINOR'})   # set by no registered profile today

    def test_table(self):
        """the table: one row per registered profile, in folder order, and dm_game set to the default's row"""
        t = game.table(game.load('010015100B514000'))
        rows = re.findall(r'^#define DM_GAME_ROW_(\d+) \{\.id = "([0-9A-F]{16})"', t, re.M)
        self.assertEqual([r[1] for r in rows], [g.id for g in REGISTERED])
        k = [g.id for g in REGISTERED].index('010015100B514000')
        self.assertIn(f'const int dm_game_nprofiles = {len(REGISTERED)};', t)
        self.assertIn(f'const int dm_game_default = {k};', t)
        self.assertIn(f'struct dm_game_profile dm_game = DM_GAME_ROW_{k};', t)
        self.assertIn('const char dm_game_default_from[] = "";', t)
        mario = next(l for l in t.split('\n') if l.startswith(f'#define DM_GAME_ROW_{k} '))
        self.assertIn('.nmod = 2, .modules = {"main", "sdk"}', mario)
        self.assertIn('.shared_font_ttf = -1', mario)


class Outside(unittest.TestCase):
    """profiles outside dm/games (DM_GAMES_DIR), through the command line as the build and the packer run it"""
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        self.env = dict(os.environ, DM_GAMES_DIR=str(self.tmp / 'profiles')); self.env.pop('DM_GAMES_FIRST', None)

    def profile(self, pid, change):   # a copy of Mario's game.tsv under DM_GAMES_DIR with rows replaced (key: new value) or added
        d = self.tmp / 'profiles' / pid; d.mkdir(parents=True)
        rows, left = [], dict(change)
        for l in (game.HERE / '010015100B514000/game.tsv').read_text(encoding='utf-8').split('\n'):
            k = l.split('\t', 1)[0]
            rows.append(f'{k}\t{left.pop(k)}' if k in left and not l.startswith('#') else l)
        (d / 'game.tsv').write_text('\n'.join(rows + [f'{k}\t{v}' for k, v in left.items()]) + '\n', encoding='utf-8')
        return d

    def game_py(self, *args):
        return subprocess.run([sys.executable, str(GAME_PY), *map(str, args)], env=self.env, capture_output=True, text=True)

    def test_runtime_file_of_a_provisional_profile(self):
        self.profile('0100AA0800000000', {'name': 'Provisional', 'mem_reserve': '0x150000000', 'modules': 'main', 'device_api': '53.313', 'shared_font': 'ttf'})
        out = self.tmp / 'p.tsv'
        r = self.game_py('runtime', '0100AA0800000000', out)
        self.assertEqual(r.returncode, 0, r.stderr)
        f = file_rows(out.read_text(encoding='utf-8'))
        self.assertEqual((f['id'], f['name'], f['mem_reserve'], f['modules'], f['nvn_api_major'], f['nvn_api_minor'], f['shared_font_ttf']),
                         ('0100AA0800000000', 'Provisional', '0x150000000', 'main', '53', '313', '1'))
        self.assertIn(str(self.tmp / 'profiles' / '0100AA0800000000'), out.read_text(encoding='utf-8').split('\n')[0])

    def test_table_with_an_outside_default(self):
        d = self.profile('0100AA0800000000', {'name': 'Provisional'})
        out = self.tmp / 't.c'
        r = self.game_py('table', out, d)   # CMake passes the folder game.py dir found at configure time
        self.assertEqual(r.returncode, 0, r.stderr)
        t = out.read_text(encoding='utf-8')
        n = len(REGISTERED)
        self.assertIn(f'const int dm_game_nprofiles = {n};', t)   # --game searches the registered rows only
        self.assertIn(f'const int dm_game_default = {n};', t)
        self.assertIn(f'#define DM_GAME_ROW_{n} {{.id = "0100AA0800000000", .name = "Provisional"', t)
        self.assertIn(f'const char dm_game_default_from[] = {game.cq(str(d.resolve()))};', t)

    def test_written_only_when_changed(self):
        out = self.tmp / 'h' / 'game_profile.h'
        self.assertEqual(self.game_py('header', '--runtime', out).returncode, 0)
        t0 = out.stat().st_mtime_ns; os.utime(out, ns=(t0 - 10**9, t0 - 10**9)); t1 = out.stat().st_mtime_ns
        self.assertEqual(self.game_py('header', '--runtime', out).returncode, 0)
        self.assertEqual(out.stat().st_mtime_ns, t1)
        self.assertEqual(out.read_text(encoding='utf-8'), game.runtime_header())

    def test_refusals(self):
        """what the port's file cannot carry stops game.py, naming the profile and the key"""
        cases = (('0100AA0900000000', {'name': 'Two\tcolumns'}, 'name holds a tab'),
                 ('0100AA0A00000000', {'modules': ' '.join(f'm{i}' for i in range(17))}, '17 modules'),
                 ('0100AA0B00000000', {'build': '\t'.join(['v0', '1.0 beta', 'main=ab'])}, 'display version'))
        for pid, change, want in cases:
            with self.subTest(want=want):
                self.profile(pid, change)
                r = self.game_py('runtime', pid, self.tmp / f'{pid}.tsv')
                self.assertNotEqual(r.returncode, 0)
                self.assertIn(pid, r.stderr); self.assertIn(want, r.stderr)
                self.assertFalse((self.tmp / f'{pid}.tsv').exists())


if __name__ == '__main__':
    unittest.main()
