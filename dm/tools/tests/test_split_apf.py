"""shader_split: Vector Unit APF packages ('FPUV' version 7, Beach Buggy Racing 2): stored, zlib and LZ4 block entries,
header/directory sums, amended/directory-first bounds, unknown compressions, decompression bombs and damaged packages.
Synthetic bytes only."""
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


def pack_entry(raw, comp, stored):
    if stored is not None:
        return stored
    if comp == 1:
        return zlib.compress(raw)
    if comp == 5:
        return lz4.block.compress(raw, store_size=False)
    return raw


def apf(entries, version=7):
    """Build an APF package. entries: [(name, raw, compression, stored_override_or_None)]; compression 0 stored, 1 zlib, 5 LZ4 block."""
    data, directory = bytearray(), bytearray()
    for name, raw, comp, stored in entries:
        packed = pack_entry(raw, comp, stored)
        directory += name.encode() + b'\0' + struct.pack('<4I2H', ss.APF_HEADER + len(data), len(raw), len(packed), ss.fnv1a32(raw), 32, comp)
        data += packed
    h = b'FPUV' + struct.pack('<5I', version, ss.APF_HEADER + len(data), len(entries), len(directory), ss.fnv1a32(directory)) + b'Switch'.ljust(32, b'\0')
    return h + struct.pack('<I', ss.fnv1a32(h)) + bytes(data) + bytes(directory)


def apf_amended(before_entries, after_entries, version=7):
    """Build an APF package whose directory sits between before_entries and appended after_entries (or before all data when before_entries is empty)."""
    before_data = bytearray()
    before_meta = []
    for name, raw, comp, stored in before_entries:
        packed = pack_entry(raw, comp, stored)
        before_meta.append((name, ss.APF_HEADER + len(before_data), len(raw), len(packed), ss.fnv1a32(raw), comp))
        before_data += packed
    dir_off = ss.APF_HEADER + len(before_data)
    all_names = [e[0] for e in before_entries] + [e[0] for e in after_entries]
    dir_len = sum(len(nm.encode()) + 21 for nm in all_names)
    after_data = bytearray()
    after_meta = []
    for name, raw, comp, stored in after_entries:
        packed = pack_entry(raw, comp, stored)
        after_meta.append((name, dir_off + dir_len + len(after_data), len(raw), len(packed), ss.fnv1a32(raw), comp))
        after_data += packed
    directory = bytearray()
    for name, off, usz, csz, hsum, comp in before_meta + after_meta:
        directory += name.encode() + b'\0' + struct.pack('<4I2H', off, usz, csz, hsum, 32, comp)
    assert len(directory) == dir_len
    h = b'FPUV' + struct.pack('<5I', version, dir_off, len(all_names), dir_len, ss.fnv1a32(directory)) + b'Switch'.ljust(32, b'\0')
    return h + struct.pack('<I', ss.fnv1a32(h)) + bytes(before_data) + bytes(directory) + bytes(after_data)


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

    def test_amended_and_directory_first_packages_are_scanned(self):
        vs1, ps1 = stage(fz.VS, '1200000000000001'), stage(fz.PS, '2300000000000002')
        vs2, ps2 = stage(fz.VS, '3400000000000003'), stage(fz.PS, '4500000000000004')
        amended = apf_amended(
            [('VuShaderAsset/Base/A', shader_asset([fz.glslc([vs1, ps1])]), 5, None)],
            [('VuShaderAsset/Patch/B', shader_asset([fz.glslc([vs2, ps2])]), 1, None)],
        )
        (_, keys, pairs, extra), log = self.scan('Amended.apf', amended)
        self.assertEqual((log, extra), ('', {}))
        self.assertEqual(set(keys), {ss.skey(*vs1), ss.skey(*ps1), ss.skey(*vs2), ss.skey(*ps2)})
        self.assertEqual(pairs, {(ss.skey(*vs1), ss.skey(*ps1)), (ss.skey(*vs2), ss.skey(*ps2))})

        dir_first = apf_amended([], [('VuShaderAsset/Fx/Only', shader_asset([fz.glslc([vs1])]), 5, None)])
        (_, keys2, _, extra2), log2 = self.scan('DirFirst.apf', dir_first)
        self.assertEqual((log2, extra2, set(keys2)), ('', {}, {ss.skey(*vs1)}))

    def test_duplicate_spans_are_decoded_once_and_zero_offset_empty_entry_is_skipped(self):
        vs1 = stage(fz.VS, '5600000000000005')
        raw = shader_asset([fz.glslc([vs1])])
        packed = lz4.block.compress(raw, store_size=False)
        directory = (
            b'VuShaderAsset/Empty\0' + struct.pack('<4I2H', 0, 0, 0, ss.fnv1a32(b''), 32, 0)
            + b'VuShaderAsset/First\0' + struct.pack('<4I2H', ss.APF_HEADER, len(raw), len(packed), ss.fnv1a32(raw), 32, 5)
            + b'VuShaderAsset/Alias\0' + struct.pack('<4I2H', ss.APF_HEADER, len(raw), len(packed), ss.fnv1a32(raw), 32, 5)
        )
        h = b'FPUV' + struct.pack('<5I', 7, ss.APF_HEADER + len(packed), 3, len(directory), ss.fnv1a32(directory)) + b'Switch'.ljust(32, b'\0')
        blob = h + struct.pack('<I', ss.fnv1a32(h)) + packed + directory
        with mock.patch.object(lz4.block, 'decompress', wraps=lz4.block.decompress) as spy:
            (_, keys, _, extra), log = self.scan('dup.apf', blob)
        self.assertEqual(spy.call_count, 1)
        self.assertEqual((log, extra, set(keys)), ('', {}, {ss.skey(*vs1)}))
        self.assertEqual(keys[ss.skey(*vs1)][0], '/dup.apf/VuShaderAsset/First#GLSLC@0x18')

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

    def test_lz4_and_zlib_bombs_stop_without_unbounded_allocation(self):
        lz4_bomb = apf([('VuShaderAsset/X', bytes(0x10000), 5, bytes(4))])
        with mock.patch.object(lz4.block, 'decompress', wraps=lz4.block.decompress) as spy:
            (_, keys, _, extra), log = self.scan('lz4bomb.apf', lz4_bomb)
        self.assertEqual(spy.call_count, 0)
        self.assertEqual((keys, extra[0]), ({}, 'unreadable'))
        self.assertIn('255 to 1', extra[1])
        self.assertTrue(log.startswith('[RED] /lz4bomb.apf/VuShaderAsset/X:'), log)

        # Declared usz exceeding RFC 1951 max ratio (1032:1) stops before zlib even starts.
        zlib_ratio_bomb = apf([('VuShaderAsset/Z', bytes(0x10000), 1, bytes(4))])
        with mock.patch.object(ss.zlib, 'decompressobj', wraps=zlib.decompressobj) as spy:
            (_, keys, _, extra), log = self.scan('zbomb_ratio.apf', zlib_ratio_bomb)
        self.assertEqual(spy.call_count, 0)
        self.assertEqual((keys, extra[0]), ({}, 'unreadable'))
        self.assertIn('1032 to 1', extra[1])

        # Small declared usz with a compressed payload that would inflate far larger stops at usz + 1.
        huge_packed = zlib.compress(bytes(1 << 20))
        zlib_inflate_bomb = apf([('VuShaderAsset/Z', bytes(0x20), 1, huge_packed)])
        (_, keys, _, extra), log = self.scan('zbomb_len.apf', zlib_inflate_bomb)
        self.assertEqual((keys, extra[0]), ({}, 'unreadable'))
        self.assertIn('0x21 bytes decoded, the entry says 0x20', extra[1])

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

        def patch_entry_off(pkg, new_off):
            d = bytearray(pkg)
            dir_at, _, dir_len = struct.unpack_from('<3I', d, 8)
            struct.pack_into('<I', d, dir_at + len('VuShaderAsset/X') + 1, new_off)
            struct.pack_into('<I', d, 0x14, ss.fnv1a32(d[dir_at:dir_at + dir_len]))
            return resum(d)

        amended = apf_amended([], [('VuShaderAsset/X', b'1234', 0, None)])
        amended_dir_at, _, amended_dir_len = struct.unpack_from('<3I', amended, 8)
        cases = (
            ('short', good[:0x20], 'shorter than a Vector Unit APF header'),
            ('version', version, 'Vector Unit APF version 8 (only version 7 is read)'),
            ('header', bytes(header), 'the APF header does not match its FNV-1a sum'),
            ('directory', bytes(directory), 'the APF directory does not match its FNV-1a sum'),
            ('outside', resum(outside), 'lies outside the file'),
            ('entry_in_dir', patch_entry_off(good, at), 'lie outside the APF data'),
            ('entry_overlaps_dir_end', patch_entry_off(amended, amended_dir_at + amended_dir_len - 2), 'lie outside the APF data'),
            ('entry_past_eof', patch_entry_off(amended, len(amended) - 2), 'lie outside the APF data'),
            ('entry_in_hdr', patch_entry_off(good, 0x10), 'lie outside the APF data'),
            ('bad_lz4', apf([('VuShaderAsset/X', bytes(0x100), 5, b'\xff' * 0x40)]), 'not an LZ4 block'),
            ('bad_zlib', apf([('VuShaderAsset/X', bytes(0x100), 1, b'not zlib')]), 'invalid zlib stream'),
            ('bad_stored_len', apf([('VuShaderAsset/X', bytes(0x10), 0, b'1234')]), 'stored bytes, the entry says 0x10'),
            ('zero_usz_nonzero_csz', apf([('VuShaderAsset/X', b'', 0, b'1234')]), 'stored bytes, the entry says 0x0'),
            ('nonzero_usz_zero_csz', apf([('VuShaderAsset/X', bytes(0x10), 0, b'')]), '0x0 stored bytes, the entry says 0x10'),
            ('bad_zlib_len', apf([('VuShaderAsset/X', bytes(0x100), 1, zlib.compress(bytes(0x80)))]), '0x80 bytes decoded, the entry says 0x100'),
            ('zlib_trailing', apf([('VuShaderAsset/X', bytes(0x80), 1, zlib.compress(bytes(0x80)) + b'junk')]), '0x80 bytes decoded, the entry says 0x80'),
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
