"""Mods for a build: the code patch formats (exefs_mods.py) and finding and staging mods in the layouts downloads come in (mods.py); synthetic files only."""
import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))


def load(name):
    spec = importlib.util.spec_from_file_location(name, REPO / f'dm/tools/{name}.py')
    assert spec is not None and spec.loader is not None
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m


ex, mods = load('exefs_mods'), load('mods')
TID = '01006F8002326000'
BID = 'ff1d1c05670db6021c85b624a710b963'


def write(root, rel, data=b'x'):
    p = Path(root) / rel; p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data if isinstance(data, bytes) else data.encode()); return p


class Pchtxt(unittest.TestCase):
    def test_entries_shift_and_blocks(self):
        with tempfile.TemporaryDirectory() as d:
            p = write(d, 'a.pchtxt', f'@nsobid-{BID.upper()}\n@flag print_values\n@flag offset_shift 0x100\n\n// disabled first\n0000aa00 00000000\n'
                                     '@enabled\n0000bb00 1f2003d5 // nop\n0000cc04 E0031F2A\n@disabled\n0000dd00 00\n@enabled\n@stop\n0000ee00 00\n')
            bid, entries, lines = ex.parse_pchtxt(str(p))
            self.assertEqual(bid, BID)
            self.assertEqual(entries, [(0xbb00, bytes.fromhex('1f2003d5')), (0xcc04, bytes.fromhex('e0031f2a'))])   # shift 0x100 - header 0x100: the entry is the module offset
            self.assertEqual([ln for ln, _ in lines], [8, 9])

    def test_no_shift_counts_the_header(self):
        with tempfile.TemporaryDirectory() as d:
            p = write(d, 'a.pchtxt', f'@nsobid-{BID}\n@enabled\n00000200 00\n')
            self.assertEqual(ex.parse_pchtxt(str(p))[1], [(0x100, b'\0')])

    def test_errors(self):
        bad = {'entries before the @nsobid': '@enabled\n0100 00\n', 'unsupported flag': f'@nsobid-{BID}\n@flag nonsense 1\n',
               'not an "<offset> <hex bytes>"': f'@nsobid-{BID}\n@enabled\n0100 "text"\n', 'no @nsobid': '// nothing\n',
               'in the NSO header': f'@nsobid-{BID}\n@enabled\n00000010 00\n', 'unsupported directive': f'@nsobid-{BID}\n@other\n'}
        with tempfile.TemporaryDirectory() as d:
            for i, (why, text) in enumerate(bad.items()):
                with self.subTest(why), self.assertRaisesRegex(ex.ModError, why): ex.parse_pchtxt(str(write(d, f'{i}.pchtxt', text)))


class Ips(unittest.TestCase):
    def test_ips_records_and_runs(self):
        body = b'PATCH' + (0x1000).to_bytes(3, 'big') + struct.pack('>H', 4) + b'\x1f\x20\x03\xd5' + (0x2000).to_bytes(3, 'big') + b'\0\0' + struct.pack('>H', 3) + b'\xaa' + b'EOF'
        with tempfile.TemporaryDirectory() as d:
            bid, entries = ex.parse(str(write(d, f'{BID.upper()}.ips', body)))
            self.assertEqual(bid, BID)
            self.assertEqual(entries, [(0xf00, b'\x1f\x20\x03\xd5'), (0x1f00, b'\xaa\xaa\xaa')])

    def test_ips32_and_long_build_id(self):
        body = b'IPS32' + struct.pack('>IH', 0x01000100, 2) + b'\x01\x02' + b'EEOF'
        with tempfile.TemporaryDirectory() as d:
            bid, entries = ex.parse(str(write(d, 'A226048605D3597F90CB1067D141CD36647B90CD.ips', body)))
            self.assertEqual(bid, 'a226048605d3597f90cb1067d141cd36')   # the first 16 bytes
            self.assertEqual(entries, [(0x01000000, b'\x01\x02')])

    def test_bad_files(self):
        with tempfile.TemporaryDirectory() as d:
            for name, data, why in (('x.ips', b'PATCH', 'not a build ID'), (f'{BID}.ips', b'NOPE', 'neither IPS'), (f'{BID}.ips', b'PATCH\x00\x10', 'cut off'),
                                    (f'{BID}.ips', b'PATCH\x00\x00\x10\x00\x01\x00EOF', 'NSO header')):
                with self.subTest(why), self.assertRaisesRegex(ex.ModError, why): ex.parse(str(write(d, name, data)))

    def test_matches(self):
        self.assertTrue(ex.matches(BID[:16], BID)); self.assertTrue(ex.matches(BID, BID.upper())); self.assertFalse(ex.matches('00' + BID[2:], BID))


class Finding(unittest.TestCase):
    def setUp(self):
        self.t = tempfile.TemporaryDirectory(); self.d = Path(self.t.name)

    def tearDown(self): self.t.cleanup()

    def names(self, folder): return [(m.name, len(m.patches), bool(m.romfs)) for m in mods.find(folder, TID)]

    def test_a_mod_itself(self):
        write(self.d, 'Speed/romfs/Pack/StaticParam.pack'); write(self.d, 'Speed/exefs/speedfix_main.pchtxt', f'@nsobid-{BID}\n')
        self.assertEqual(self.names(self.d / 'Speed'), [('Speed', 1, True)])

    def test_ryujinx_download(self):   # the 60 fps mod's ryujinx folder: three variants under contents/<title ID>/
        for v in ('ACNH 45 FPS + SpeedFix', 'ACNH 60 FPS + SpeedFix'):
            write(self.d, f'ryujinx/mods/contents/{TID}/{v}/romfs/Pack/StaticParam.pack'); write(self.d, f'ryujinx/mods/contents/{TID}/{v}/exefs/enabler_sdk.pchtxt')
        self.assertEqual(self.names(self.d / 'ryujinx/mods'), [('ACNH 45 FPS + SpeedFix', 1, True), ('ACNH 60 FPS + SpeedFix', 1, True)])
        self.assertEqual(len(self.names(self.d / 'ryujinx')), 2)   # the tree one level down (a folder of mods)
        self.assertEqual(self.names(self.d), [])   # two levels down: not looked for

    def test_atmosphere_tree(self):   # romfs under contents/<title ID>, code patches under exefs_patches/<mod>/<build ID>.ips (title ID folder in lower case)
        write(self.d, f'Pack/atmosphere/contents/{TID.lower()}/romfs/a.bin'); write(self.d, f'Pack/atmosphere/exefs_patches/Fps/{BID}.ips', b'PATCHEOF')
        self.assertEqual(self.names(self.d / 'Pack'), [('Pack', 0, True), ('Fps', 1, False)])
        self.assertEqual(self.names(self.d), [('Pack', 0, True), ('Fps', 1, False)])   # a folder of mods: each subfolder looked into

    def test_patch_folder_and_yuzu(self):
        write(self.d, f'exefs_patches/Fps/{BID}.ips', b'PATCHEOF'); write(self.d, f'load/{TID}/Hd/romfs/a.bin')
        self.assertEqual(self.names(self.d / 'exefs_patches/Fps'), [('Fps', 1, False)])
        self.assertEqual(self.names(self.d / 'load'), [('Hd', 0, True)])

    def test_unsupported_modules_and_other_games(self):
        write(self.d, 'Exl/exefs/subsdk9'); write(self.d, 'Exl/exefs/main.npdm'); write(self.d, 'Other/atmosphere/contents/0100000000010000/romfs/a')
        m = mods.find(self.d / 'Exl', TID)[0]
        self.assertEqual([p.name for p in m.other], ['main.npdm', 'subsdk9'])
        self.assertEqual(mods.find(self.d / 'Other', TID), [])
        with self.assertRaises(FileNotFoundError): mods.find(self.d / 'missing', TID)

    def test_stage(self):
        a = write(self.d, 'src/Speed/exefs/speedfix_main.pchtxt'); write(self.d, 'src/Speed/romfs/Pack/x.pack', 'p'); write(self.d, 'src/Look/romfs/a/b.bin', 'b')
        write(self.d, 'src/Speed/env.txt', 'DM_PRESENT_INTERVAL=display\n')
        write(self.d, 'src/Unused/exefs/old.pchtxt')
        found = mods.find(self.d / 'src', TID)
        out = self.d / 'out'; write(out, 'stale/romfs/x')
        rows = mods.stage(found, {str(a)}, out)
        self.assertEqual(rows, [('Look', 1, 0), ('Speed', 1, 1), ('Unused', 0, 0)])
        self.assertFalse((out / 'stale').exists())
        self.assertEqual((out / 'Look/romfs/a/b.bin').read_text(), 'b')
        self.assertTrue((out / 'Speed/dm_code.tsv').read_text().endswith('speedfix_main.pchtxt\n'))
        self.assertFalse((out / 'Look/dm_code.tsv').exists())
        self.assertEqual((out / 'Speed/env.txt').read_text(), 'DM_PRESENT_INTERVAL=display\n')

    def test_applied_files(self):
        write(self.d, 'p/main.mods.tsv', '# header\n0x10\t00\t11\t/m/a.pchtxt\n0x20\t00\t11\t/m/b.ips\n'); write(self.d, 'p/sdk.mods.tsv', '0x0\t00\t11\t/m/c.pchtxt\n')
        self.assertEqual(mods.applied_files(self.d / 'p'), {'/m/a.pchtxt', '/m/b.ips', '/m/c.pchtxt'})


if __name__ == '__main__':
    unittest.main()
