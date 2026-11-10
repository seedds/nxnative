"""Telltale .ttarch2 archives (Sam & Max: The Devil's Playhouse), without game data: ZCTT blocks of raw-deflate chunks, an effect pack of bare GLSLC records."""
from pathlib import Path
import os
import struct
import sys
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
os.environ.setdefault('DM_GAME', '0100D5501857E000')
import shader_split

CTL, GFX = shader_split.CTL_MAGIC, shader_split.GFX_MAGIC


def deflate(b):
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    return c.compress(b) + c.flush()


def zctt(data, chunk=64):
    """A ZCTT block of `data` in chunks of `chunk` bytes: magic, version, count, then count + 1 u64 offsets from its start"""
    parts = [deflate(data[i:i + chunk]) for i in range(0, len(data), chunk)]
    head = 12 + 8 * (len(parts) + 1)
    offs, at = [], head
    for p in parts: offs.append(at); at += len(p)
    return b'ZCTT' + struct.pack('<II', 0x10000, len(parts)) + struct.pack(f'<{len(parts) + 1}Q', *offs, at) + b''.join(parts)


def control(code_len, nctl=0x880, compute=False):
    c = bytearray(nctl)
    c[:4] = CTL
    struct.pack_into('<I', c, 0x20, nctl)
    struct.pack_into('<I', c, 0x704, code_len)
    if compute: struct.pack_into('<3I', c, 0x748, 8, 8, 1)
    return bytes(c)


def code_area(n):
    """A graphics code area: the magic, and SPH word 0 (+0x30) with shader type 5, a pixel shader"""
    c = bytearray(n)
    c[:4] = GFX
    struct.pack_into('<I', c, 0x30, 5 << 10)
    return bytes(c)


def record(code, ctl): return code + struct.pack('<I', len(ctl)) + ctl


class Ttarch2Tests(unittest.TestCase):
    def pack(self, *records, pad=3):
        return b'\0' * pad + b''.join(records)

    def test_ctl_blocks_roundtrip(self):
        data = os.urandom(1000)
        d = zctt(data)
        offs = shader_split.ttcz_chunks(d)
        self.assertEqual(len(offs), 1000 // 64 + 2)
        self.assertEqual(shader_split.ttcz_inflate(d, 0, offs), data)
        self.assertEqual(shader_split.ttcz_inflate(d, 0, offs, first=1), data[:64])

    def test_not_a_block(self):
        self.assertIsNone(shader_split.ttcz_chunks(b'ZCTT\0\0\1\0'))
        self.assertIsNone(shader_split.ttcz_chunks(b'NCTT' + bytes(32)))
        bad = bytearray(zctt(b'x' * 200)); struct.pack_into('<Q', bad, 12, 7)   # first offset is not the header's length
        self.assertIsNone(shader_split.ttcz_chunks(bytes(bad)))

    def test_archive_without_an_effect_pack_is_left_alone(self):
        pack = self.pack(record(code_area(0x80), control(0x80)))
        self.assertEqual(shader_split.ttarch2_shaders(zctt(b'4ATT' + b'texture.d3dtx' + pack), 'x'), [])

    def test_records_in_a_zctt_and_an_nctt(self):
        for compute in (False, True):
            code = bytes(0x80) if compute else code_area(0x80)
            ctl = control(len(code), compute=compute)
            body = b'4ATT\0\0\1\0' + b'SM3_nx_Shaders_mem.t3fxpack' + self.pack(record(code, ctl), pad=5)
            nctt = b'NCTT' + struct.pack('<Q', 0) + b'SM3_nx_Shaders_mem.t3fxpack'.ljust(0x40, b'\0') + zctt(body)
            for archive in (zctt(body), nctt):
                with self.subTest(compute=compute, kind=archive[:4]):
                    got = shader_split.ttarch2_shaders(archive, '/a')
                    self.assertEqual([(s, c, k) for _, s, c, k in got], [('cs' if compute else 'ps', ctl, code)])
                    self.assertIn('#TTFX@', got[0][0])

    def test_a_record_whose_length_word_disagrees_is_dropped(self):
        ctl = control(0x80)
        bad = code_area(0x80) + struct.pack('<I', 0x123) + ctl
        body = b'SM3_nx_Shaders_mem.t3fxpack' + self.pack(bad)
        self.assertEqual(shader_split.ttarch2_shaders(zctt(body), '/a'), [])


if __name__ == '__main__':
    unittest.main()
