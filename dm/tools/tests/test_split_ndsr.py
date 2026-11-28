"""NDSR record-start scoring reads complete records, including in short packs. Synthetic bytes only."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'fuzz'))
import fuzz_shader_split as fz

ss = fz.ss


def pack(start, stages):
    """Both supported header layouts, followed by a zero record and minimal NVN2 shaders."""
    data = bytearray(0x40); data[:4] = b'NDSR'
    shaders = []
    for i, sph in enumerate(stages):
        ctl = bytearray(0x48); ctl[:8] = bytes.fromhex('34127698c9000000')
        code = struct.pack('<I', sph) + bytes(0x7c)
        struct.pack_into('<I', ctl, 0x44, len(code))
        offset = len(data)
        struct.pack_into('<4I', data, start + 16 * i, offset, len(ctl) << 4, offset + len(ctl), len(code) << 4)
        data += ctl + code
        shaders.append((offset, bytes(ctl), code))
    return bytes(data), shaders


class NdsrRecords(unittest.TestCase):
    def tearDown(self):
        ss.BAHS_VAR.clear()

    def test_short_empty_packs_do_not_raise_on_partial_records(self):
        for size in range(4, 0x125):
            with self.subTest(size=size):
                self.assertEqual(ss.ndsr_shaders(b'NDSR' + bytes(size - 4), '/empty'), [])

    def test_short_shader_is_kept_for_both_record_starts(self):
        for start in (0x10, 0x14):
            data, [(offset, ctl, code)] = pack(start, [0x0461])
            self.assertEqual(len(data), 0x108)
            for tail in range(16):
                with self.subTest(start=start, tail=tail):
                    self.assertEqual(ss.ndsr_shaders(data + bytes(tail), '/short'),
                                     [(f'/short#NDSR@{offset:#x}', 'vs', ctl, code)])

    def test_vertex_pixel_pair_is_preserved_for_both_record_starts(self):
        for start in (0x10, 0x14):
            with self.subTest(start=start):
                data, shaders = pack(start, [0x0461, 0x1462])
                found = ss.ndsr_shaders(data, '/pair')
                self.assertEqual(found, [(f'/pair#NDSR@{offset:#x}', stage, ctl, code)
                                         for stage, (offset, ctl, code) in zip(('vs', 'ps'), shaders)])
                self.assertEqual(ss.BAHS_VAR, {'/pair#NDSR@0x40': '/pair#NDSR#0x40',
                                              '/pair#NDSR@0x108': '/pair#NDSR#0x40'})

    def test_pack_inside_a_monolith_xbc1_stream(self):
        # Xenoblade Chronicles 2's Switch 2 Edition: /monolib/shader/ndsr.pck is an XBC1 member of bf2.ard, read through monolith_shaders
        import zlib
        data, shaders = pack(0x10, [0x0461, 0x1462])
        packed = zlib.compress(data, level=0)
        xbc = struct.pack('<4s4I28s', b'xbc1', 1, len(data), len(packed), 0, b'ndsr.pck') + packed
        found = []; ss.monolith_shaders(xbc, '/bf2.ard/monolib/shader/ndsr.pck', found)
        self.assertEqual([(st, ctl, code) for _, st, ctl, code in found],
                         [(stage, ctl, code) for stage, (_, ctl, code) in zip(('vs', 'ps'), shaders)])


if __name__ == '__main__':
    unittest.main()
