"""shader_split: a Liquid Engine archive (Frostpunk: <name>.idx / .str / .dat) is read through its index, and every entry of its .dat that is a shader file reaches the split
(liquid_index, liquid_shaders, liquid_shader), whatever its name; its .str and .dat are not scanned again as loose files. Synthetic bytes only."""
import contextlib
import io
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools/fuzz'))
import fuzz_shader_split as fz   # noqa: E402
ss = fz.ss
zstd = ss.zstd_mod()

HEAD = lambda n: bytes((0, 5, 2)) + struct.pack('<II', n, 0)   # noqa: E731  # the 11 bytes the index and the names start with


def shader(ctl, code, version=4, trailer=b'v\0\0\0|Platform.hlsl|Math.hlsl\0'):
    """One shader file: magic, version, two file times, code + control + 8, code length, control length, the code area, the control section, the include list"""
    return struct.pack('<IIQQIII', 0xcdbe5660, version, 1, 2, len(code) + len(ctl) + 8, len(code), len(ctl)) + code + ctl + trailer


def longer(ctl, code):
    """Another shader of the same stage: one more instruction word, and the control section's code length to match"""
    code = code + bytes.fromhex('0f00000000000000'); ctl = bytearray(ctl); struct.pack_into('<I', ctl, 0x704, len(code))
    return bytes(ctl), code


def archive(directory, name, entries):
    """<name>.idx / .str / .dat in directory: entries = [(entry name, data, zstd)], hashed by their index"""
    dat, idx, names = bytearray(), bytearray(HEAD(len(entries))), bytearray(HEAD(len(entries)))
    for i, (nm, data, packed) in enumerate(entries):
        stored = zstd.compress(data) if packed else data
        h = 0x100000 + i * 0x1111
        idx += struct.pack('<IQQQB', h, len(stored), len(data), len(dat), 1 if packed else 0); dat += stored
        names += struct.pack('<II', h, len(nm)) + nm.encode()
    for ext, data in (('.idx', idx), ('.str', names), ('.dat', dat)): (Path(directory) / (name + ext)).write_bytes(data)
    return str(Path(directory) / (name + '.idx'))


class LiquidArchives(unittest.TestCase):
    def scan(self, entries, name='common'):
        with tempfile.TemporaryDirectory() as t, contextlib.redirect_stdout(io.StringIO()) as log:
            path = archive(t, name, entries)
            self.assertEqual(ss.liquid_index(path), len(entries))
            result = ss.scan_file((path, name + '.idx'))
        return result, log.getvalue()

    def test_shader_entries_reach_the_split_by_what_they_are(self):
        (rel, keys, pairs, extra), log = self.scan([
            ('shaders/compiled/simpleshader/00080900000000000000000000000000.vs_5_0', shader(*fz.VS), True),
            ('shaders/compiled/simpleshader/00080900000000000000000000000000.ps_5_0', shader(*fz.PS), False),   # stored as it is
            ('scripts/startup.lua', b'print("not a shader")\n' * 8, True),
            ('renamed.bin', shader(*longer(*fz.VS)), True)])   # a shader is found whatever its name
        self.assertEqual(log, ''); self.assertEqual(extra, {})
        self.assertEqual(sorted(v[1] for v in keys.values()), ['ps', 'vs', 'vs'])
        self.assertTrue(all('#LIQUID@' in v[0] and v[0].startswith('/common.dat/') for v in keys.values()))
        self.assertIn('/common.dat/shaders/compiled/simpleshader/00080900000000000000000000000000.ps_5_0#LIQUID@0x24', {v[0] for v in keys.values()})
        self.assertEqual(pairs, set())   # one stage each, combined per draw (separable_pairs covers them)
        self.assertTrue(any(s in '#LIQUID@0x24' for s in ss.SINGLE_STAGE))

    def test_what_is_not_an_index(self):
        with tempfile.TemporaryDirectory() as t:
            path = archive(t, 'common', [('a.vs_5_0', shader(*fz.VS), True)])
            os.unlink(Path(t) / 'common.dat'); self.assertIsNone(ss.liquid_index(path))   # no .dat beside it
            path = archive(t, 'other', [('a.vs_5_0', shader(*fz.VS), True)])
            with open(path, 'ab') as f: f.write(b'\0')
            self.assertIsNone(ss.liquid_index(path))   # its length is not that of its entries
            self.assertIsNone(ss.liquid_index(str(Path(t) / 'other.str')))

    def test_a_shader_file_of_another_layout_is_a_red_line(self):
        ctl, code = fz.VS
        (rel, keys, pairs, extra), log = self.scan([('a.vs_5_0', shader(ctl, code, version=5), True)])
        self.assertEqual(keys, {}); self.assertEqual(extra[0], 'unreadable'); self.assertIn('[RED]', log); self.assertIn('version 5', log)
        bad = bytearray(shader(ctl, code)); struct.pack_into('<I', bad, 0x24 + len(code) + 0x704, len(code) + 8)   # the control section names another code length
        (rel, keys, pairs, extra), log = self.scan([('b.vs_5_0', bytes(bad), False)])
        self.assertEqual(keys, {}); self.assertIn('not a shader', log)

    def test_cli_packs_the_archives_shaders(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); rom = root / 'RomFS'; rom.mkdir(); main = root / 'main'; main.write_bytes(b'NSO0' + bytes(0xfc))
            archive(rom, 'common', [('shaders/compiled/x/0.vs_5_0', shader(*fz.VS), True), ('shaders/compiled/x/0.ps_5_0', shader(*fz.PS), True)])
            archive(rom, 'voices', [])
            dest = root / 'out'; dest.mkdir(); outputs = [dest / n for n in ('pkg', 'map.tsv', 'facts.tsv', 'pairs.tsv', 'pso.tsv')]
            proc = subprocess.run([sys.executable, REPO / 'dm/gfx/tools/shader_split.py', rom, main, *outputs], capture_output=True, text=True, env=dict(os.environ, DM_GAME='0400E88025B1A000', J='2'))
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr); self.assertNotIn('[RED]', proc.stdout + proc.stderr)
            self.assertIn('shaders: 2 (', proc.stdout)
            m = (dest / 'map.tsv').read_text()
            self.assertIn('Liquid Engine shaders', m); self.assertIn('/common.dat/shaders/compiled/x/0.vs_5_0#LIQUID@0x24', m)


if __name__ == '__main__': unittest.main()
