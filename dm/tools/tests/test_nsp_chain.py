"""NSP script chain files (scripts/build_ipa_from_nsp.py chain_header, chain_tables): the one chain version header (dm/gfx/nvn/dmsh_chain.h) is written again when stale and left
alone when current, and the committed one is current; only the c1 / uniform extent tables that are missing (they are not committed) or do not name the library's stamp (its chain version
and profile inputs) are made (GitHub issue #78), none without a stamped library, and no step's reuse key counts them; the tools that make them read only the shader build's outputs
(an exFAT drive's AppleDouble `._<name>` beside each, PR #555); no game files, keys or Xcode needed (temporary files only in a
temporary folder)."""
import importlib.util
import json
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
SCRIPT = REPO / 'scripts/build_ipa_from_nsp.py'
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', SCRIPT)
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        self.said = []
        for cm in (mock.patch.object(nsp, 'PROG', mock.Mock()), mock.patch.object(nsp, 'say', self.said.append)):
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)


class Header(Base):
    def setUp(self):   # a repository whose chain tools are the real ones (linked) and whose header is its own
        super().setUp()
        self.repo = self.tmp / 'repo'
        (self.repo / 'dm/gfx/nvn').mkdir(parents=True)
        (self.repo / 'dm/gfx/tools').symlink_to(REPO / 'dm/gfx/tools'); (self.repo / 'dm/ios').symlink_to(REPO / 'dm/ios')
        (self.repo / 'dm/third_party').symlink_to(REPO / 'dm/third_party')
        (self.repo / 'dm/mac').symlink_to(REPO / 'dm/mac')   # min_os.mk, the Mac libraries' deployment target
        (self.repo / 'dm/gfx/nvn/dmsh_abi.h').symlink_to(REPO / 'dm/gfx/nvn/dmsh_abi.h')   # the shader library ABI, a chain input beside the header
        self.h = self.repo / nsp.CHAIN_H
        shutil.copy(REPO / nsp.CHAIN_H, self.h)
        self.env = {k: v for k, v in os.environ.items() if k != 'DM_GAME'}   # the version needs no game

    def header(self):
        nsp.chain_header(sys.executable, self.repo, self.env, self.tmp / 'logs')

    def test_committed_current(self):   # a chain change committed without dmsh_chain.py --header
        r = subprocess.run([sys.executable, str(REPO / 'dm/gfx/tools/dmsh_chain.py')], env=self.env, capture_output=True, text=True, check=True)
        self.assertEqual(nsp.chain_of(REPO), r.stdout.split()[-1], 'dm/gfx/nvn/dmsh_chain.h is stale: run dm/gfx/tools/dmsh_chain.py --header and commit it')

    def test_stale_rewritten(self):
        want = nsp.chain_of(REPO)
        self.h.write_text('#define DMSH_CHAIN "000000000000"\n')
        self.header()
        self.assertEqual(nsp.chain_of(self.repo), want)
        self.assertEqual(len(self.said), 1); self.assertIn('held 000000000000', self.said[0])

    def test_missing_written(self):
        want = nsp.chain_of(REPO)
        self.h.unlink()
        self.header()
        self.assertEqual(nsp.chain_of(self.repo), want)
        self.assertIn('held no version', self.said[0])

    def test_current_untouched(self):
        os.utime(self.h, (1, 1))
        self.header()
        self.assertEqual(self.h.stat().st_mtime, 1); self.assertEqual(self.said, [])


class Tables(Base):
    CHAIN, INPUTS = '0123456789ab', 'aaaaaaaaaaaa'

    def setUp(self):
        super().setUp()
        self.sh = self.tmp / 'shader'; self.lib = self.sh / 'cache' / f'b-{self.CHAIN}'; self.lib.mkdir(parents=True)
        (self.lib / 'inputs.tsv').write_text(f'# stamp\ngame\t010014E00DB56000\ninputs\t{self.INPUTS}\n')
        self.g = types.SimpleNamespace(table=lambda n: self.tmp / n)
        self.ran = []
        cm = mock.patch.object(nsp, 'run', lambda cmd, log, env=None: self.ran.append(Path(cmd[1]).name)); cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def tables(self, c1, cb):   # each a header line's "b-<chain> inputs <digest>" part (or any text), None for a missing table
        for n, head in (('c1_full.tsv', c1), ('cb_extent.tsv', cb)):
            if head: (self.tmp / n).write_text(f'# chain {head} 2026-10-23T00:00:00+00:00\n')
        return nsp.chain_tables(sys.executable, REPO, self.sh, {}, self.tmp / 'logs', self.g, self.CHAIN)

    def current(self): return f'b-{self.CHAIN} inputs {self.INPUTS}'

    def test_current_kept(self):
        self.assertTrue(self.tables(self.current(), self.current()))
        self.assertEqual(self.ran, []); self.assertEqual(self.said, [])

    def test_other_inputs_made_again(self):   # the profile's tables changed and a full build stamped the library again
        self.assertTrue(self.tables(self.current(), f'b-{self.CHAIN} inputs bbbbbbbbbbbb'))
        self.assertEqual(self.ran, ['cb_extent.py']); self.assertIn(f'named b-{self.CHAIN} inputs bbbbbbbbbbbb', self.said[0])

    def test_made_again_with_the_shader_builds_memo(self):   # c1_guard.py / cb_extent.py DMSH_TABLE_MEMO: the folder a full build gives them
        envs = []
        with mock.patch.object(nsp, 'run', lambda cmd, log, env=None: envs.append(env)): self.tables(None, self.current())
        self.assertEqual([e['DMSH_TABLE_MEMO'] for e in envs], [str(self.sh / 'cache')])

    def test_other_chain_made_again(self):
        self.assertTrue(self.tables(f'b-fc6c0368c7b9 inputs {self.INPUTS}', self.current()))
        self.assertEqual(self.ran, ['c1_guard.py'])

    def test_old_header_made_again(self):   # a table from before the stamps names only the chain
        self.assertTrue(self.tables(f'b-{self.CHAIN}', self.current()))
        self.assertEqual(self.ran, ['c1_guard.py'])

    def test_missing_made(self):   # a checkout's first pack: the tables are not in the repository
        self.assertTrue(self.tables(None, None))
        self.assertEqual(self.ran, ['c1_guard.py', 'cb_extent.py']); self.assertIn('c1_full.tsv missing', self.said[0])

    def test_no_stamp(self):   # no full build made these Mac outputs: build_shaders then builds again (reuse) or stops (after a build)
        (self.lib / 'inputs.tsv').unlink()
        self.assertFalse(self.tables(self.current(), self.current()))
        self.assertEqual(self.ran, [])

    def test_no_mac_outputs(self):
        shutil.rmtree(self.sh / 'cache')
        self.assertFalse(self.tables(None, None))
        self.assertEqual(self.ran, [])

    def test_not_profile_inputs(self):   # the steps' reuse keys leave out what the iOS shader step makes, and the rewriter's patches (the rewrite step's own key)
        for n in ('game.tsv', 'shader_map.tsv', 'c1_full.tsv', 'cb_extent.tsv', 'patches.py'): (self.tmp / n).write_text('')
        self.assertEqual([p.name for p in nsp.profile_files(types.SimpleNamespace(dir=self.tmp))], ['game.tsv', 'shader_map.tsv'])


class TableTools(unittest.TestCase):
    def test_appledouble_files_are_not_shaders(self):   # macOS writes a binary ._<name> beside each file on an exFAT / FAT drive; c1_guard.py read ._<key>_b.metal as UTF-8 and stopped
        lib = Path(tempfile.mkdtemp()) / 'b-0123456789ab'; lib.mkdir(); self.addCleanup(shutil.rmtree, lib.parent)
        key = '00112233aabbccdd'
        (lib / f'{key}_b.metal').write_text('fragment float4 main0(constant cb& fp_c1) { int _5 = in.v; return (fp_c1)._m0[_5]; }\n')
        (lib / f'{key}_b.json').write_text(json.dumps({'bindings': [{'kind': 'cbuf', 'reg': 0, 'msl_name': 'fp_c1'}]}))
        for n in (f'{key}_b.metal', f'{key}_b.json', 'inputs.tsv'): (lib / ('._' + n)).write_bytes(b'\0\x05\x16\x07\0\x02\0\0' + b'\xa0' * 4088)   # not UTF-8, not JSON
        for tool, table in (('c1_guard.py', 'c1_full.tsv'), ('cb_extent.py', 'cb_extent.tsv')):
            with self.subTest(tool=tool):
                r = subprocess.run([sys.executable, REPO / 'dm/gfx/tools' / tool, lib, lib.parent / table], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual((lib.parent / 'c1_full.tsv').read_text().splitlines()[1:], [key])   # the shader itself is still checked

    UNBOUNDED = 'fragment float4 main0(constant cb& fp_c1) { int _5 = in.v; return (fp_c1)._m0[_5]; }\n'
    BOUNDED = 'fragment float4 main0(constant cb& fp_c1) { int _5 = in.v & 3; return (fp_c1)._m0[_5]; }\n'

    def memo_lib(self):   # two shaders: one c1 read unbounded, one masked to the first 64 bytes of a 64-byte c1; both read cbuf slot 0
        lib = Path(tempfile.mkdtemp()) / 'b-0123456789ab'; lib.mkdir(); self.addCleanup(shutil.rmtree, lib.parent)
        refl = json.dumps({'c1': '00' * 64, 'bindings': [{'kind': 'cbuf', 'reg': 0, 'msl_name': 'fp_c1'}]})
        for key, src in (('00112233aabbccdd', self.UNBOUNDED), ('44556677eeff0011', self.BOUNDED)):
            (lib / f'{key}_b.metal').write_text(src); (lib / f'{key}_b.json').write_text(refl)
        return lib

    def table(self, lib, tool, memo):   # the table's lines after its header (the header holds the time)
        table = lib.parent / f'{tool}.tsv'
        env = {k: v for k, v in os.environ.items() if k != 'DMSH_TABLE_MEMO'}
        if memo: env['DMSH_TABLE_MEMO'] = str(memo)
        r = subprocess.run([sys.executable, REPO / 'dm/gfx/tools' / f'{tool}.py', lib, table], capture_output=True, text=True, env=env)
        self.assertEqual(r.returncode, 0, r.stderr)
        return table.read_text().splitlines()[1:]

    def test_memo_gives_the_same_tables(self):   # DMSH_TABLE_MEMO: a reuse build's tables from the memo are the ones the analysis makes
        lib = self.memo_lib(); memo = lib.parent / 'memo'
        for tool in ('c1_guard', 'cb_extent'):
            with self.subTest(tool=tool):
                plain = self.table(lib, tool, None)
                self.assertFalse((memo / f'{tool}.memo').exists())
                self.assertEqual(self.table(lib, tool, memo), plain)   # empty memo: analysed, then kept
                lines = (memo / f'{tool}.memo').read_text().splitlines()
                self.assertTrue(lines[0].startswith('# ')); self.assertEqual(len(lines), 3)
                self.assertEqual(self.table(lib, tool, memo), plain)   # from the memo
        self.assertEqual(self.table(lib, 'c1_guard', None), ['00112233aabbccdd'])

    def test_memo_answers_are_used_and_keyed_by_the_files(self):
        lib = self.memo_lib(); memo = lib.parent / 'memo'
        self.table(lib, 'c1_guard', memo)
        path = memo / 'c1_guard.memo'; head, *rows = path.read_text().splitlines()
        path.write_text(head + '\n' + ''.join(r.split('\t')[0] + '\tu\n' for r in rows))   # every shader answered "not proven"
        self.assertEqual(self.table(lib, 'c1_guard', memo), ['00112233aabbccdd', '44556677eeff0011'])   # a warm run takes the memo's answer
        (lib / '44556677eeff0011_b.metal').write_text(self.BOUNDED.replace('& 3', '& 3 '))   # changed bytes: a new digest, analysed again
        self.assertEqual(self.table(lib, 'c1_guard', memo), ['00112233aabbccdd'])
        self.assertEqual(len(path.read_text().splitlines()), 3)   # only this run's answers are kept

    def test_memo_of_other_tools_is_not_taken(self):   # another version of the tools (its first line) starts the memo empty
        lib = self.memo_lib(); memo = lib.parent / 'memo'
        self.table(lib, 'c1_guard', memo)
        path = memo / 'c1_guard.memo'; head, *rows = path.read_text().splitlines()
        path.write_text('# another\n' + ''.join(r.split('\t')[0] + '\tu\n' for r in rows))
        self.assertEqual(self.table(lib, 'c1_guard', memo), ['00112233aabbccdd'])
        self.assertEqual(path.read_text().splitlines()[0], head)

    def test_memo_of_another_abi_header_is_not_taken(self):   # cb_extent.py's binding kinds come from dmsh_abi.h through dmsh_abi.py: a header-only edit kept a warm memo's row
        tree = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, tree)
        for f in ('dm/gfx/tools/c1_guard.py', 'dm/gfx/tools/cb_extent.py', 'dm/gfx/tools/dmsh_abi.py', 'dm/gfx/nvn/dmsh_abi.h', 'dm/tools/romfs.py', 'dm/tools/shader_lib.py'):
            (tree / f).parent.mkdir(parents=True, exist_ok=True); shutil.copy(REPO / f, tree / f)
        lib = Path(tempfile.mkdtemp()) / 'b-0123456789ab'; lib.mkdir(); self.addCleanup(shutil.rmtree, lib.parent)
        (lib / '00112233aabbccdd_b.metal').write_text('struct S { constant cb* fp_c1 [[id(0)]]; };\n'
                                                     'fragment float4 main0(constant S& spvDescriptorSet0) { int _5 = in.v & 3; return (*spvDescriptorSet0.fp_c1)._m0[_5]; }\n')
        (lib / '00112233aabbccdd_b.json').write_text(json.dumps({'c1': '00' * 64, 'bindings': [{'kind': 'cbuf', 'reg': 0, 'msl_name': 'fp_c1'}]}))
        env = dict(os.environ, DMSH_TABLE_MEMO=str(lib.parent / 'memo'))

        def rows():
            table = lib.parent / 'cb_extent.tsv'
            r = subprocess.run([sys.executable, tree / 'dm/gfx/tools/cb_extent.py', lib, table], capture_output=True, text=True, env=env)
            self.assertEqual(r.returncode, 0, r.stderr)
            return [line for line in table.read_text().splitlines() if not line.startswith('#')]
        self.assertEqual(rows(), ['00112233aabbccdd\t0:64'])
        header = tree / 'dm/gfx/nvn/dmsh_abi.h'; text = header.read_text()
        self.assertEqual(text.count('#define DMSH_KIND_CBUF "cbuf"'), 1)
        header.write_text(text.replace('#define DMSH_KIND_CBUF "cbuf"', '#define DMSH_KIND_CBUF "cbuf_renamed"'))   # the reflection's "cbuf" is no longer a uniform buffer
        self.assertEqual(rows(), [])


class TableDifference(unittest.TestCase):   # the row the build error names when a split table differs from the profile's (GitHub issue #1263)
    HEAD = b'# header\n'

    def test_changed_row(self):
        got = nsp.table_difference('shader_map.tsv', self.HEAD + b'a\nvs\t1\n', b'# another header\na\nms\t0\n')
        self.assertEqual(got, 'shader_map.tsv: 1 row(s) only in the profile, 1 only in the split\n  profile: vs\t1\n  split:   ms\t0')

    def test_inserted_row_is_the_only_one_named(self):
        got = nsp.table_difference('t.tsv', self.HEAD + b'a\nb\nc\n', self.HEAD + b'new\na\nb\nc\n')
        self.assertEqual(got, 't.tsv: 0 row(s) only in the profile, 1 only in the split\n  split:   new')

    def test_repeated_row_counts(self):
        self.assertIn('1 row(s) only in the profile', nsp.table_difference('t.tsv', b'a\na\n', b'a\n'))

    def test_order_only(self):
        self.assertEqual(nsp.table_difference('t.tsv', b'a\nb\n', b'b\na\n'), 't.tsv: the same rows in another order')



class TranslatorKey(Base):   # the shader step's key takes the translator by its bytes: a commit switched and back rewrote the same bytes with a new mtime, and each worktree has its own path; either rebuilt every shader (PR #2496)
    class Keyed(Exception): pass

    def key(self, xlate):   # build_shaders' reuse key, with the other inputs fixed
        keys = []
        def reused(_kf, key): keys.append(key); raise self.Keyed
        g = types.SimpleNamespace(id='0100000000000000', table=lambda n: self.tmp / 'profile' / n)
        with mock.patch.multiple(nsp, reused=reused, chain_of=lambda repo: '0123456789ab', chain_files=lambda repo: [], toolchain=lambda repo, env=None: (),
                                 shader_profile_key=lambda *a: ('shader inputs', g.id, 'digest')), mock.patch.object(nsp.shader_cache, 'adopt'):
            with self.assertRaises(self.Keyed): nsp.build_shaders(sys.executable, xlate.parents[4], self.tmp / 'sh', xlate, {}, self.tmp / 'logs', g, pkg_key='pkg')
        return keys[0]

    def translator(self, checkout, data):
        x = self.tmp / checkout / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'; x.parent.mkdir(parents=True, exist_ok=True); x.write_bytes(data)
        return x

    def test_same_bytes_new_mtime_keep_the_key(self):
        x = self.translator('a', b'translator v1'); before = self.key(x); st = x.stat()
        x.write_bytes(b'translator v1'); os.utime(x, ns=(st.st_atime_ns, st.st_mtime_ns + 5_000_000_000))
        self.assertEqual(self.key(x), before)

    def test_another_checkout_keeps_the_key(self):   # a second worktree: the same bytes at another path
        self.assertEqual(self.key(self.translator('a', b'translator v1')), self.key(self.translator('b', b'translator v1')))

    def test_other_bytes_change_it(self):
        x = self.translator('a', b'translator v1'); before = self.key(x); x.write_bytes(b'translator v2')
        self.assertNotEqual(self.key(x), before)

if __name__ == '__main__':
    unittest.main()
