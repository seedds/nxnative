"""shader_split: Vector Unit APF packages ('FPUV' version 7, Beach Buggy Racing 2): stored, zlib and LZ4 block entries, the header and directory sums,
unknown compressions and damaged packages. Synthetic bytes only."""
import contextlib
import io
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest import mock

import lz4.block

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
sys.path.insert(0, str(REPO / 'dm/tools/fuzz'))
import fuzz_shader_split as fz
import romfs_image

ss = fz.ss


def stage(original, word):
    ctl, code = original
    return ctl, code[:0x80] + bytes.fromhex(word) * 8


def shader_asset(outputs):
    """A VuShaderAsset's data as the game has it: a 0x18-byte header (output count, lengths, sums), then the GLSLC outputs."""
    body = b''.join(outputs)
    return struct.pack('<6I', len(outputs), len(body), 0x7403b5d1, len(body), 0xbfb5a3fa, len(body) - 0x10) + body


def apf(entries, version=7):
    """Build an APF package. entries: [(name, raw, compression, stored_override_or_None)]; compression 0 stored, 1 zlib, 5 LZ4 block."""
    data, directory = bytearray(), bytearray()
    for name, raw, comp, stored in entries:
        if stored is not None:
            packed = stored
        elif comp == 1:
            packed = zlib.compress(raw)
        elif comp == 5:
            packed = lz4.block.compress(raw, store_size=False)
        else:
            packed = raw
        directory += name.encode() + b'\0' + struct.pack('<4I2H', ss.APF_HEADER + len(data), len(raw), len(packed), ss.fnv1a32(raw), 32, comp)
        data += packed
    h = b'FPUV' + struct.pack('<5I', version, ss.APF_HEADER + len(data), len(entries), len(directory), ss.fnv1a32(directory)) + b'Switch'.ljust(32, b'\0')
    return h + struct.pack('<I', ss.fnv1a32(h)) + bytes(data) + bytes(directory)


def resum(d):
    """The package with its header sum made right again after a header field was changed."""
    d = bytearray(d)
    struct.pack_into('<I', d, 0x38, ss.fnv1a32(d[:0x38]))
    return bytes(d)


class Apf(unittest.TestCase):
    def scan(self, name, data):
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()) as log:
            path = Path(tmp) / name
            path.write_bytes(data)
            result = ss.scan_file((str(path), name))
        return result, log.getvalue()

    def test_fnv1a32(self):
        self.assertEqual(ss.fnv1a32(b''), 0x811c9dc5)
        self.assertEqual(ss.fnv1a32(b'a'), 0xe40c292c)
        self.assertEqual(ss.fnv1a32(b'foobar'), 0xbf9cf968)

    def test_entries_stored_zlib_lz4_are_scanned_and_unknown_compression_warns(self):
        vs1, ps1 = stage(fz.VS, '1100000000000001'), stage(fz.PS, '2200000000000002')
        vs2, ps2 = stage(fz.VS, '3300000000000003'), stage(fz.PS, '4400000000000004')
        a = fz.glslc([vs1, ps1])
        blob = apf([
            ('Assets/AssetData', b'\1\0\0\0en\0', 1, None),
            ('VuShaderAsset/Fx/A', shader_asset([a]), 5, None),
            ('VuShaderAsset/Fx/B', shader_asset([fz.glslc([vs2])]), 1, None),
            ('VuShaderAsset/Fx/Empty', b'', 5, b''),
            ('VuTextureAsset/T', bytes(0x400), 9, None),
            ('VuShaderAsset/Fx/C', shader_asset([fz.glslc([ps2])]), 0, None),
        ])
        (rel, keys, pairs, extra), log = self.scan('Assets.apf', blob)
        self.assertEqual((rel, extra), ('Assets.apf', {}))
        self.assertEqual(set(keys), {ss.skey(*vs1), ss.skey(*ps1), ss.skey(*vs2), ss.skey(*ps2)})
        self.assertTrue(keys[ss.skey(*vs1)][0].startswith('/Assets.apf/VuShaderAsset/Fx/A#GLSLC@0x18'), keys[ss.skey(*vs1)])
        self.assertEqual(keys[ss.skey(*vs2)][0], '/Assets.apf/VuShaderAsset/Fx/B#GLSLC@0x18')
        self.assertEqual(keys[ss.skey(*ps2)][0], '/Assets.apf/VuShaderAsset/Fx/C#GLSLC@0x18')
        self.assertEqual(pairs, {(ss.skey(*vs1), ss.skey(*ps1))})
        warns = [line for line in log.splitlines() if line.startswith('[WARN] /Assets.apf:')]
        self.assertEqual(len(warns), 1, log)
        self.assertIn('9 × 1', warns[0])

    def test_any_file_name_and_a_romfs_image_end_to_end(self):
        vs1, ps1 = stage(fz.VS, '5500000000000005'), stage(fz.PS, '6600000000000006')
        blob = apf([('VuShaderAsset/Fx/A', shader_asset([fz.glslc([vs1, ps1])]), 5, None)])
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            romfs = root / 'RomFS'
            romfs.mkdir()
            (romfs / 'Assets.pak').write_bytes(blob)
            image = root / 'RomFS.img'
            romfs_image.pack(romfs, image)
            main = root / 'main'
            main.write_bytes(b'NSO0' + bytes(0xfc))
            out = [root / name for name in ('pkg', 'map.tsv', 'facts.tsv', 'pairs.tsv', 'pso.tsv')]
            r = subprocess.run(
                [sys.executable, REPO / 'dm/gfx/tools/shader_split.py', image, main, *out],
                env=dict(os.environ, DM_GAME='01007BB017812000', J='2'),
                capture_output=True, text=True,
            )
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertNotIn('[RED]', r.stdout)
            rows = [line.split('\t') for line in out[1].read_text().splitlines() if not line.startswith('#')]
            got = {row[3]: row[4] for row in rows if row[0].endswith('.bin')}
            self.assertEqual(set(got), {ss.skey(*vs1), ss.skey(*ps1)})
            self.assertTrue(all(src.startswith('/Assets.pak/VuShaderAsset/Fx/A#GLSLC@0x18') for src in got.values()), got)

    def test_lz4_bomb_stops_before_decompress(self):
        blob = apf([('VuShaderAsset/X', bytes(0x10000), 5, bytes(4))])
        with mock.patch.object(lz4.block, 'decompress', wraps=lz4.block.decompress) as spy:
            (_, keys, _, extra), log = self.scan('bomb.apf', blob)
        self.assertEqual(spy.call_count, 0)
        self.assertEqual((keys, extra[0]), ({}, 'unreadable'))
        self.assertIn('255 to 1', extra[1])
        self.assertTrue(log.startswith('[RED] /bomb.apf/VuShaderAsset/X:'), log)

    def test_large_count_is_refused_before_the_directory_is_read(self):
        good = apf([('VuShaderAsset/X', b'1234', 0, None)])
        bad = bytearray(good)
        struct.pack_into('<I', bad, 0xc, 0xffffffff)
        bad = resum(bad)
        with mock.patch.object(ss, 'fnv1a32', wraps=ss.fnv1a32) as spy:
            (_, keys, _, extra), _ = self.scan('count.apf', bad)
        self.assertEqual(spy.call_count, 1)   # the header's sum only
        self.assertEqual((keys, extra[0]), ({}, 'unreadable'))
        self.assertIn('more than fit in its', extra[1])

    def test_damaged_packages_are_reported_as_unreadable(self):
        good = apf([('VuShaderAsset/X', b'1234', 0, None)])
        at = struct.unpack_from('<I', good, 8)[0]
        version = resum(good[:4] + struct.pack('<I', 8) + good[8:])
        header = bytearray(good); header[0x20] ^= 1
        directory = bytearray(good); directory[at + 3] ^= 1
        outside = bytearray(good); struct.pack_into('<I', outside, 0x10, len(good))
        entry = bytearray(good)
        struct.pack_into('<I', entry, at + len('VuShaderAsset/X') + 1, at)   # its data would start where the directory does
        struct.pack_into('<I', entry, 0x14, ss.fnv1a32(entry[at:]))
        cases = (
            ('short', good[:0x20], 'shorter than a Vector Unit APF header'),
            ('version', version, 'Vector Unit APF version 8 (only version 7 is read)'),
            ('header', bytes(header), 'the APF header does not match its FNV-1a sum'),
            ('directory', bytes(directory), 'the APF directory does not match its FNV-1a sum'),
            ('outside', resum(outside), 'lies outside the file'),
            ('entry', resum(entry), 'lie outside the APF data'),
            ('bad_lz4', apf([('VuShaderAsset/X', bytes(0x100), 5, b'\xff' * 0x40)]), 'not an LZ4 block'),
            ('bad_zlib', apf([('VuShaderAsset/X', bytes(0x100), 1, b'not zlib')]), 'invalid zlib stream'),
            ('bad_stored_len', apf([('VuShaderAsset/X', bytes(0x10), 0, b'1234')]), 'stored bytes, the entry says 0x10'),
            ('bad_zlib_len', apf([('VuShaderAsset/X', bytes(0x100), 1, zlib.compress(bytes(0x80)))]), '0x80 bytes decoded, the entry says 0x100'),
        )
        for label, data, why in cases:
            with self.subTest(case=label):
                (_, keys, _, extra), log = self.scan(f'{label}.apf', data)
                self.assertEqual(keys, {})
                self.assertEqual(extra[0], 'unreadable')
                self.assertIn(why, extra[1])
                self.assertTrue(log.startswith(f'[RED] /{label}.apf'), log)

    def test_fpuv_is_only_an_apf_package_at_the_start(self):
        vs1 = stage(fz.VS, '7700000000000007')
        blob = b'xxxxFPUV' + fz.glslc([vs1])
        (_, keys, _, extra), log = self.scan('plain.bin', blob)
        self.assertEqual((set(keys), extra, log), ({ss.skey(*vs1)}, {}, ''))


if __name__ == '__main__':
    unittest.main()
