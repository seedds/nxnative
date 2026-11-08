"""RomFS tweaks (dm/tools/romfs_tweaks.py): a profile's rows rewrite the numbers under a BYML key in a zstd SARC, from a RomFS folder or image, into <out>/<VAR>/<value>/<path>;
a row that changes nothing or a file the RomFS lacks stops the build. The files here are made up: nothing of a game."""
import importlib.util
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('romfs_tweaks', ROOT / 'dm/tools/romfs_tweaks.py')
assert spec and spec.loader
tweaks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tweaks)
zstd = tweaks.zstd


def byml(entries):   # a version 7 BYML whose root dictionary holds {key: (type, raw 32-bit value)}, keys sorted as BYML wants them
    keys = sorted(entries)
    def strtab(strs):
        offs, pool = [], b''
        base = 4 + 4 * (len(strs) + 1)
        for s in strs: offs.append(base + len(pool)); pool += s.encode() + b'\0'
        offs.append(base + len(pool))
        t = struct.pack('<I', 0xC2 | (len(strs) << 8)) + struct.pack(f'<{len(offs)}I', *offs) + pool
        return t + b'\0' * (-len(t) % 4)
    kt = strtab(keys)
    root = struct.pack('<I', 0xC1 | (len(keys) << 8)) + b''.join(struct.pack('<IBBBB', i, 0, 0, 0, 0)[:3] + bytes([entries[k][0]]) + struct.pack('<I', entries[k][1]) for i, k in enumerate(keys))
    head = 16
    return b'YB' + struct.pack('<HIII', 7, head, 0, head + len(kt)) + kt + root


def f32(v): return struct.unpack('<I', struct.pack('<f', v))[0]


def sarc(members):   # a little-endian SARC of {name: bytes}
    names, sfat, data = b'', b'', b''
    for nm, d in members.items():
        data += b'\0' * (-len(data) % 8)
        sfat += struct.pack('<IIII', 0, (1 << 24) | (len(names) // 4), len(data), len(data) + len(d))
        data += d; names += nm.encode() + b'\0'; names += b'\0' * (-len(names) % 4)
    head = b'SFAT' + struct.pack('<HHI', 12, len(members), 0x65) + sfat + b'SFNT' + struct.pack('<HH', 8, 0) + names
    doff = 0x14 + len(head); doff += -doff % 8
    out = b'SARC' + struct.pack('<HHII', 0x14, 0xFEFF, doff + len(data), doff) + struct.pack('<HH', 0x100, 0) + head
    return out + b'\0' * (doff - len(out)) + data


class RomfsTweaksTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        rom = self.tmp / 'romfs/Deco'; rom.mkdir(parents=True)
        arc = sarc({'A.bgyml': byml({'FarClipDistance': (0xD2, f32(200.0)), 'Name': (0xD1, 7)}),
                    'B.bgyml': byml({'FarClipDistance': (0xD2, f32(80.0)), 'Margin': (0xD2, f32(50.0))})})
        (rom / 'Models.alsarc.zs').write_bytes(zstd.compress(arc))
        self.tsv = self.tmp / 'romfs_tweaks.tsv'

    def run_tool(self, rows):
        self.tsv.write_text('# test\n' + ''.join('\t'.join(r) + '\n' for r in rows), encoding='utf-8')
        return subprocess.run([sys.executable, ROOT / 'dm/tools/romfs_tweaks.py', self.tsv, self.tmp / 'romfs', self.tmp / 'out'], capture_output=True, text=True)

    def values(self, var, value):   # {member: {key: value}} of the rewritten file
        b = zstd.decompress((self.tmp / 'out' / var / value / 'Deco/Models.alsarc.zs').read_bytes())
        out = {}
        for nm, (s, e) in tweaks.sarc_members(b).items():
            out[nm] = {k: round(struct.unpack_from('<f' if t == 0xD2 else '<i', b, at)[0], 3) for k, t, at in tweaks.byml_scalars(b, s)}
        return out

    def test_scale_and_set_in_every_member(self):
        r = self.run_tool([('DM_DECO_DISTANCE', '0.5', 'Deco/Models.alsarc.zs', 'FarClipDistance', 'scale', '0.5'),
                           ('DM_DECO_DISTANCE', '0.5', 'Deco/Models.alsarc.zs', 'Margin', 'set', '15')])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.values('DM_DECO_DISTANCE', '0.5'), {'A.bgyml': {'FarClipDistance': 100.0, 'Name': 7}, 'B.bgyml': {'FarClipDistance': 40.0, 'Margin': 15.0}})

    def test_a_row_that_changes_nothing_stops_the_build(self):   # another game version: the key moved or was renamed
        r = self.run_tool([('DM_DECO_DISTANCE', '0.5', 'Deco/Models.alsarc.zs', 'NoSuchKey', 'scale', '0.5')])
        self.assertNotEqual(r.returncode, 0); self.assertIn('has no NoSuchKey', r.stderr)
        r = self.run_tool([('DM_DECO_DISTANCE', '0.5', 'Deco/Missing.alsarc.zs', 'FarClipDistance', 'scale', '0.5')])
        self.assertNotEqual(r.returncode, 0); self.assertIn('is not in', r.stderr)

    def test_values_are_folder_names(self):
        r = self.run_tool([('DM_DECO_DISTANCE', '../x', 'Deco/Models.alsarc.zs', 'FarClipDistance', 'scale', '0.5')])
        self.assertNotEqual(r.returncode, 0)

    def test_every_profile_table_is_well_formed(self):   # each profile's rows parse: DM_ variables, folder-name values, scale or set
        for t in (ROOT / 'dm/games').glob('*/romfs_tweaks.tsv'):
            with self.subTest(game=t.parent.name):
                self.assertTrue(tweaks.rows(t))


if __name__ == '__main__':
    unittest.main()
