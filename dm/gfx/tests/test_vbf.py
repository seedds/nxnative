"""VBF block decoding and unaligned Phyre NVN programs, without game files."""
from pathlib import Path
import os
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
os.environ.setdefault('DM_GAME', '010015100B514000')
import shader_split as split
import vbf


def program(stage):
    code = bytearray(0x100); code[:4] = split.GFX_MAGIC
    struct.pack_into('<I', code, 0x30, {'vs': 1, 'ps': 5}[stage] << 10)
    ctl = bytearray(0x880); ctl[:4] = split.CTL_MAGIC
    struct.pack_into('<I', ctl, 0x20, len(ctl))
    struct.pack_into('<I', ctl, 0x704, len(code))
    return bytes(code + ctl)


class Vbf(unittest.TestCase):
    def test_raw_compressed_and_shared_members(self):
        payload = bytes(range(256)) * 256 + b'last block' * 100
        compressed = zlib.compress(payload[65536:])
        names = b'a.fx.phyre\0shared.fx.phyre\0'
        hlen = 16 + 2 * 64 + 4 + len(names) + 6
        header = bytearray(struct.pack('<4sIQ', b'SRYK', hlen, 2) + bytes(64))
        header += struct.pack('<IIQQQ', 0, 0, len(payload), hlen, 0)
        header += struct.pack('<IIQQQ', 2, 0, 5, 0xffffffffffffffff, 11)
        header += struct.pack('<I', len(names) + 4) + names + struct.pack('<3H', 0, len(compressed), 5)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'test.vbf'; path.write_bytes(header + payload[:65536] + compressed)
            entries = vbf.entries(path)
            self.assertEqual(len(entries), 1)
            with path.open('rb') as f: self.assertEqual(vbf.read(f, entries[0]), payload)
            path.write_bytes(path.read_bytes()[:-2])
            with path.open('rb') as f:
                with self.assertRaises(ValueError): vbf.read(f, entries[0])

    def test_lz4_blocks(self):
        """FFXII TZA compresses its blocks as raw LZ4; FFX/X-2's zlib blocks still read the same"""
        import lz4.block
        payload = b'Language=cn\r\n' * 6000 + bytes(range(256)) * 40
        first, rest = payload[:65536], payload[65536:]
        c1, c2 = lz4.block.compress(first, store_size=False), lz4.block.compress(rest, store_size=False)
        names = b'gamedata/nxgamesetting.ini\0'
        hlen = 16 + 64 + 4 + len(names) + 4
        header = bytearray(struct.pack('<4sIQ', b'SRYK', hlen, 1) + bytes(32))
        header += struct.pack('<IIQQQ', 0, 0, len(payload), hlen, 0)
        header += struct.pack('<I', len(names) + 4) + names + struct.pack('<2H', len(c1), len(c2))
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'lz4.vbf'; path.write_bytes(header + c1 + c2)
            entries = vbf.entries(path)
            with path.open('rb') as f: self.assertEqual(vbf.read(f, entries[0]), payload)
            path.write_bytes(header + c1 + b'\x00' + c2[1:])   # a match before any output: no LZ4 block
            with path.open('rb') as f:
                with self.assertRaises(ValueError): vbf.read(f, entries[0])
        self.assertEqual(vbf.block(zlib.compress(first), 65536, 'z'), first)
        with self.assertRaises(ValueError): vbf.block(b'\x00' * 10, 100, 'bad')

    def test_effect_and_loose_programs_use_same_key(self):
        vs, ps = program('vs'), program('ps')
        effect = b'RYHP' + bytes(9) + vs + bytes(7) + ps
        found = split.phyre_shaders(effect, 'effect')
        self.assertEqual([x[1] for x in found], ['vs', 'ps'])
        loose = struct.pack('<II', 0x100, 0x880) + vs
        self.assertEqual(split.skey(*found[0][2:]), split.skey(*split.phyre_shaders(loose, 'loose')[0][2:]))
        bad = bytearray(effect); struct.pack_into('<I', bad, 13 + 0x100 + 0x704, 0x104)
        self.assertEqual([x[1] for x in split.phyre_shaders(bad, 'bad')], ['ps'])
        self.assertEqual(split.phyre_shaders(b'RYHP' + split.CTL_MAGIC, 'short'), [])

    def test_phyre_programs_in_main(self):
        """FFXII TZA: Phyre code area + control section pairs in main, unaligned, outside any RYHP container, read only for a Phyre game"""
        vs, ps = program('vs'), program('ps')
        main = bytes(0x13) + vs + bytes(9) + ps + bytes(5)
        self.assertEqual([x[1] for x in split.phyre_main(main, 'main', [('/x/FFXII_TZA.vbf', 'FFXII_TZA.vbf')])], ['vs', 'ps'])
        self.assertEqual(split.phyre_main(main, 'main', [('/x/Data/level0', 'Data/level0')]), [])
        found = split.phyre_main(main, 'main', [('/x/a.fx.phyre', 'a.fx.phyre')])
        self.assertEqual(split.raw_shaders(main, 'main', {c[:struct.unpack_from('<I', c, 0x20)[0]] for _, _, c, _ in found}), [])


if __name__ == '__main__': unittest.main()
