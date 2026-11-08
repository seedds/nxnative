"""NSP script, the shader package checked against the shader map before dmsh_build.py translates (scripts/build_ipa_from_nsp.py package_map_mismatch, build_shaders): dmsh_build.py reads a
shader's record by its map index alone, so a package from another shader_split.py run than the map translated every later row from another shader's bytes (issue #1841: Ocarina of
Time's present pass ran a terrain pixel shader, black screen). The record keys are shader_split.py's own (skey), for NVN, NVN2 and a profile's shader_key_skip. No game data."""
import importlib.util
import os
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
os.environ.setdefault('DM_GAME', '0400486024DEC000')   # shader_split.py loads a profile at import; its keys use KEY_SKIP, patched per test
import shader_split as split   # noqa: E402
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
assert spec is not None and spec.loader is not None
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)


def nvn(seed, n=0x850):   # an NVN control section (its own length at +0x20) and a code area
    ctl = bytearray((seed * 7 + i) & 0xff for i in range(n)); ctl[:4] = split.CTL_MAGIC; struct.pack_into('<I', ctl, 0x20, n)
    return bytes(ctl), bytes((seed + i) & 0xff for i in range(0x100))


def nvn2(seed):   # an NVN2 control section (version 0xc9; +0x30 differs between copies of one shader)
    ctl = bytearray((seed * 5 + i) & 0xff for i in range(0xd8)); ctl[:4] = split.CTL_MAGIC; ctl[4:8] = b'\xc9\0\0\0'
    return bytes(ctl), bytes((seed * 3 + i) & 0xff for i in range(0x180))


def record(ctl, code):   # as shader_split.py writes a package's record
    rec = bytearray(b'BTCDBTCD' + bytes(0x30)); struct.pack_into('<2I', rec, 0x28, len(ctl), len(code))
    return bytes(rec + code + ctl)


class PackageMap(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.pkg = self.tmp / 'pkg'; self.pkg.mkdir()
        self.shaders = [nvn(1), nvn(2, 0x880), nvn2(3), nvn2(4), nvn(5)]

    def write(self, shaders, rows=None, skip=(0, 0)):
        with mock.patch.object(split, 'KEY_SKIP', skip):
            keys = [split.skey(c, k) for c, k in shaders]
        (self.pkg / 'ps_lib.bin').write_bytes(b''.join(record(c, k) for c, k in shaders))
        (self.tmp / 'shader_map.tsv').write_text('# kind: evidence\npackage\tindex\tstage\tdm_ctl_sha16\tsource\n' + ''.join(f'ps_lib.bin\t{i}\tpixel\t{k}\t/x#{i}\n' for i, k in (rows or list(enumerate(keys)))))
        return keys

    def test_same_run(self):   # NVN and NVN2 records, keyed as shader_split.py keys them
        self.write(self.shaders)
        self.assertEqual(nsp.package_map_mismatch(self.pkg, self.tmp / 'shader_map.tsv'), [])

    def test_key_skip(self):   # a profile's shader_key_skip: the masked bytes differ from the keyed copy and the key stays
        keys = self.write(self.shaders, skip=(0x770, 0x78))
        changed = [(bytes(c[:0x780]) + b'\xee' * 8 + bytes(c[0x788:]), k) if len(c) >= 0x850 else (c, k) for c, k in self.shaders]   # bytes inside 0x770..0x7e8 of the NVN sections
        (self.pkg / 'ps_lib.bin').write_bytes(b''.join(record(c, k) for c, k in changed))
        with mock.patch.object(split, 'KEY_SKIP', (0x770, 0x78)): self.assertEqual([split.skey(c, k) for c, k in changed], keys)
        self.assertEqual(nsp.package_map_mismatch(self.pkg, self.tmp / 'shader_map.tsv', '0x770+0x78'), [])
        self.assertNotEqual(nsp.package_map_mismatch(self.pkg, self.tmp / 'shader_map.tsv'), [])   # read without the profile's skip, the NVN keys differ

    def test_stale_package(self):   # #1841: the map from a split that dropped a record, the package from the one before
        keys = self.write(self.shaders)
        rows = list(enumerate(keys[:1] + keys[2:]))   # the new split's map: shader 1 gone, the rows after it renumbered
        (self.tmp / 'shader_map.tsv').write_text(''.join(f'ps_lib.bin\t{i}\tpixel\t{k}\t/x#{i}\n' for i, k in rows))
        bad = nsp.package_map_mismatch(self.pkg, self.tmp / 'shader_map.tsv')
        self.assertEqual([(n, i, k) for n, i, k, _ in bad], [('ps_lib.bin', i, k) for i, k in rows[1:]])
        self.assertEqual(bad[0][3], keys[1])   # row 1 would have translated shader 1's bytes

    def test_rows_dmsh_build_reads(self):   # a row without its source column, and a key's later row, are not dmsh_build.py's: not checked
        keys = self.write(self.shaders)
        (self.tmp / 'shader_map.tsv').write_text(f'ps_lib.bin\t0\tpixel\t{keys[0]}\t/x\nps_lib.bin\t3\tpixel\t{keys[0]}\t/y\nps_lib.bin\t2\tpixel\t{keys[1]}\n')
        self.assertEqual(nsp.package_map_mismatch(self.pkg, self.tmp / 'shader_map.tsv'), [])

    def test_missing_record(self):
        keys = self.write(self.shaders[:2]) + ['00' * 8]
        (self.tmp / 'shader_map.tsv').write_text(''.join(f'ps_lib.bin\t{i}\tpixel\t{k}\t/x\n' for i, k in enumerate(keys)))
        self.assertEqual(nsp.package_map_mismatch(self.pkg, self.tmp / 'shader_map.tsv'), [('ps_lib.bin', 2, '00' * 8, 'no record')])

    def test_no_map_or_package(self):   # dmsh_build.py reports those
        self.assertEqual(nsp.package_map_mismatch(self.pkg, self.tmp / 'none.tsv'), [])
        (self.tmp / 'shader_map.tsv').write_text('vs_lib.bin\t0\tvertex\t0011223344556677\t/x\n')
        self.assertEqual(nsp.package_map_mismatch(self.pkg, self.tmp / 'shader_map.tsv'), [])

    def test_build_stops_before_translating(self):   # the shader step stops with what to remove, before its reuse and before it removes anything, and dmsh_build.py never runs
        keys = self.write(self.shaders)
        (self.tmp / 'shader_map.tsv').write_text(''.join(f'ps_lib.bin\t{i}\tpixel\t{k}\t/x\n' for i, k in enumerate(keys[1:])))
        sh = self.tmp; ran = []
        g = types.SimpleNamespace(table=lambda n: self.tmp / n, dir=self.tmp)
        keyf = sh / 'mac-b-000000000000.key'; keyf.write_text('K\nlast build\n')   # a build that would be reused
        with mock.patch.object(nsp, 'run', lambda cmd, *a, **k: ran.append(cmd)), mock.patch.object(nsp, 'chain_of', lambda repo: '000000000000'), \
             mock.patch.object(nsp, 'fingerprint', lambda *a, **k: 'K'), mock.patch.object(nsp.shader_lib, 'has_library', lambda p: True), mock.patch.object(nsp, 'chain_tables', lambda *a: True):
            with self.assertRaisesRegex(nsp.BuildError, 'different shader_split.py runs: 4 map rows'):
                nsp.build_shaders(sys.executable, REPO, sh, REPO / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate', {'WORK': str(self.tmp)}, self.tmp, g, pkg_key='P', mac=True)
        self.assertEqual(ran, [])
        self.assertTrue(keyf.is_file())   # the last build's key is left as it was


if __name__ == '__main__':
    unittest.main()
