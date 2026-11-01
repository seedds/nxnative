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

    def test_lz4_blocks(self):   # FFXII TZA's archive: LZ4 blocks, which zlib cannot read
        import lz4.block
        payload = b'RYHP' + bytes(range(256)) * 300
        blocks = [lz4.block.compress(payload[i:i + 65536], store_size=False) for i in range(0, len(payload), 65536)]
        names = b'a.fx.phyre\0'
        hlen = 16 + 64 + 4 + len(names) + 2 * len(blocks)
        header = struct.pack('<4sIQ', b'SRYK', hlen, 1) + bytes(32) + struct.pack('<IIQQQ', 0, 0, len(payload), hlen, 0)
        header += struct.pack('<I', len(names) + 4) + names + struct.pack(f'<{len(blocks)}H', *map(len, blocks))
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'test.vbf'; path.write_bytes(header + b''.join(blocks))
            entries = vbf.entries(path)
            with path.open('rb') as f: self.assertEqual(vbf.read(f, entries[0]), payload)
            data = bytearray(path.read_bytes()); data[hlen:hlen + 8] = b'\xff' * 8; path.write_bytes(data)
            with path.open('rb') as f:
                with self.assertRaises(ValueError): vbf.read(f, entries[0])

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


if __name__ == '__main__': unittest.main()
