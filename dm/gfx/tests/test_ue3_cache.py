"""Unreal Engine 3 shader caches (Batman: Arkham Knight), without game data: package chunks, shader records and the key mask (shader_key_skip)."""
import os
from pathlib import Path
import struct
import sys
import unittest
import zlib

os.environ.setdefault('DM_GAME', '010015100B514000')
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split


def shader(seed, code_len=0x100):
    """A pixel shader: a code area (graphics header, SPH stage 5) and its 0x880-byte control section"""
    code = bytearray(code_len); code[:4] = shader_split.GFX_MAGIC; struct.pack_into('<I', code, 0x30, 5 << 10); code[0x80:0x84] = struct.pack('<I', seed)
    ctl = bytearray(0x880); ctl[:4] = shader_split.CTL_MAGIC; struct.pack_into('<I', ctl, 0x704, code_len); ctl[0x7d0:0x7e8] = bytes([seed & 0xff]) * 0x18
    return bytes(code), bytes(ctl)


def record(code, ctl, extra=False):
    """A cache record: u32 length, u32 code area length, u32 control length (one more u32 in the global cache), the code area, then the control section"""
    head = struct.pack('<III', len(code) + len(ctl) + 0x10, len(code), len(ctl)) + (b'\0\0\0\0' if extra else b'')
    return head + code + ctl


def package(body, block=0x40, compress=None, wide=False):
    """An Unreal Engine 3 package of file version 864: a summary, then one compressed chunk of the body in blocks of `block` bytes"""
    blocks = [body[i:i + block] for i in range(0, len(body), block)]
    stored = [compress(b) if compress else b for b in blocks]
    chunk = (shader_split.UE3_TAG + bytes(4) if wide else shader_split.UE3_TAG) + struct.pack('<QQQ' if wide else '<III', block, sum(map(len, stored)), len(body)) + b''.join(struct.pack('<QQ' if wide else '<II', len(s), len(b)) for s, b in zip(stored, blocks))
    summary = shader_split.UE3_TAG + struct.pack('<HH', 864, 0x80e3) + b'summary.'
    return summary + chunk + b''.join(stored), summary


class Ue3CacheTests(unittest.TestCase):
    def setUp(self):
        self.real_oodle, self.real_skip = shader_split.oodle, shader_split.KEY_SKIP
        self.calls = []
        def fake_oodle(comp, n):   # stands in for the game's decoder in the Mac host (DM_OODLE_SERVE): the "compressed" block is the raw one behind a two-byte header
            self.calls.append(len(comp)); self.assertEqual(comp[:2], b'\x8c\x0a'); return comp[2:2 + n]
        shader_split.oodle = fake_oodle

    def tearDown(self):
        shader_split.oodle, shader_split.KEY_SKIP = self.real_oodle, self.real_skip

    def test_package_tag_tells_ue3_from_ue4(self):
        self.assertTrue(shader_split.ue3_package(shader_split.UE3_TAG + struct.pack('<HH', 864, 0x80e3)))
        self.assertFalse(shader_split.ue3_package(shader_split.UE3_TAG + struct.pack('<i', -7)))   # Unreal Engine 4's legacy file version

    def test_chunks_zlib_stored_and_oodle(self):
        body = b''.join(record(*shader(i)) for i in range(3))
        for compress, oodle_calls in ((zlib.compress, 0), (None, 0), (lambda b: b'\x8c\x0a' + b + b'pad', None)):
            self.calls.clear()
            d, summary = package(body, compress=compress)
            self.assertEqual(shader_split.ue3_body(d, 'p.xxx'), summary + body)
            if oodle_calls is not None: self.assertEqual(len(self.calls), oodle_calls)
            else: self.assertTrue(self.calls)

    def test_zlib_window_headers(self):
        # Valid zlib headers vary with the window: MK11 uses 0x28 0x15.
        body = b'TweakvarMap' * 30
        for wide in (False, True):
            for bits in range(9, 16):
                with self.subTest(wide=wide, window=bits):
                    def compress(b):
                        z = zlib.compressobj(wbits=bits)
                        return z.compress(b) + z.flush()
                    d, summary = package(body, block=512, compress=compress, wide=wide)
                    self.assertEqual(shader_split.ue3_body(d, 'tweakvars.xxx'), summary + body)
                    self.assertFalse(self.calls, 'zlib must not be sent to Oodle')

    def test_only_zero_padding_can_follow_the_last_chunk(self):
        body = record(*shader(1))
        for wide in (False, True):
            d, summary = package(body, wide=wide)
            self.assertEqual(shader_split.ue3_body(d + bytes(0x8000 - len(d)), 'p.xxx'), summary + body)
            for tail in (b'\0', b'\0\0\0'):
                with self.subTest(wide=wide, tail=tail):
                    self.assertEqual(shader_split.ue3_body(d + tail, 'p.xxx'), summary + body)
            for tail in (b'x', b'\0x\0', b'trailing', shader_split.UE3_TAG + b'\0'):
                with self.subTest(wide=wide, tail=tail), self.assertRaisesRegex(SystemExit, 'no compressed chunk'):
                    shader_split.ue3_body(d + tail, 'p.xxx')

    def test_wide_chunks(self):
        body = b''.join(record(*shader(i)) for i in range(3))
        for compress in (None, zlib.compress, lambda b: b'\x8c\x0a' + b + b'pad'):
            d, summary = package(body, compress=compress, wide=True)
            decoded = shader_split.ue3_body(d, 'mk11.xxx')
            self.assertEqual(decoded, summary + body)
            self.assertEqual(len(shader_split.ue3_shaders(decoded, 'mk11.xxx')), 3)

    def test_wide_truncated_table(self):
        d, summary = package(b'a' * 128, wide=True)
        with self.assertRaisesRegex(SystemExit, 'block table'):
            shader_split.ue3_body(d[:len(summary) + 40], 'mk11.xxx')

    def test_chunk_totals_checked(self):
        for wide in (False, True):
            d, summary = package(b'a' * 128, wide=wide)
            d = bytearray(d)
            struct.pack_into('<Q' if wide else '<I', d, len(summary) + (16 if wide else 8), 1)
            with self.assertRaisesRegex(SystemExit, 'totals differ'):
                shader_split.ue3_body(d, 'p.xxx')

    def test_records_of_both_caches(self):
        shaders = [shader(i, 0x100 + 0x80 * (i % 2)) for i in range(4)]
        d = b'BMSG' + b'\0' * 12 + b''.join(record(c, k, extra=i % 2 == 0) for i, (c, k) in enumerate(shaders))
        got = shader_split.ue3_shaders(d, 'g.bin')
        self.assertEqual([(st, ctl, code) for _, st, ctl, code in got], [('ps', k, c) for c, k in shaders])
        self.assertTrue(all('#UE3@' in w for w, *_ in got))   # single-stage programs (separable_pairs)

    def test_control_first_records(self):
        code, ctl = shader(1)
        ctl = bytearray(ctl + b'compiler text' * 80)
        struct.pack_into('<I', ctl, 0x20, len(ctl))
        head = struct.pack('<IIII', len(ctl), len(code), 0, len(ctl))
        data = head + ctl + code
        self.assertEqual(shader_split.ue3_shaders(data, 'mk11.xxx'), [('mk11.xxx#UE3@10', 'ps', bytes(ctl), code)])
        self.assertEqual(shader_split.ue3_shaders(data[:-1], 'cut.xxx'), [])
        bad = bytearray(data); struct.pack_into('<I', bad, 12, len(ctl) - 1)
        self.assertEqual(shader_split.ue3_shaders(bad, 'bad.xxx'), [])

    def test_control_first_extra_data_and_headerless_compute(self):
        code, ctl = shader(2)
        ctl = bytearray(ctl + b'compiler text' * 80)
        struct.pack_into('<I', ctl, 0x20, len(ctl))
        struct.pack_into('<3I', ctl, 0x748, 8, 4, 1)
        code = bytes(len(code))   # no graphics / compute code-area header
        stored = bytes(ctl) + b'additional reflection'
        data = struct.pack('<IIII', len(stored), len(code), 0, len(stored)) + stored + code
        got = shader_split.ue3_shaders(data, 'mk11.xxx')
        self.assertEqual([(st, c, k) for _, st, c, k in got], [('cs', stored, code)])

    def test_compact_records(self):
        """Batman: Arkham Asylum's compact records: {u32 code region length, u32 metadata length, u32 0}, the code region (code from its SPH, constants, a second code) and the metadata"""
        def compact(sph, csz, ksz, gsz, seed):
            region = struct.pack('<I', sph) + bytes([seed]) * (csz - 4) + b'K' * ksz + b'G' * gsz
            meta = bytearray(0x40); struct.pack_into('<III', meta, 0, 1, csz, ksz); struct.pack_into('<I', meta, 0x1c, gsz); meta[0x28:0x40] = bytes([seed + 1]) * 0x18
            return struct.pack('<III', len(region), len(meta), 0) + region + bytes(meta), region
        cases = [(0x1462, 0x1d0, 0x40, 0, 3),      # a pixel shader; GLSLC's code ends 0x30 short of a 256-byte boundary
                 (0x0461, 0xd0, 0x20, 0x60, 5)]    # a vertex shader with a second code
        records = [compact(*c) for c in cases]
        got = shader_split.ue3_shaders(b'\xff' * 7 + records[0][0] + b'\0' * 5 + records[1][0], 'g.bin')
        self.assertEqual([st for _, st, _, _ in got], ['ps', 'vs'])
        self.assertTrue(all('#UE3c@' in w for w, *_ in got))
        for (_, st, ctl, code), (_, region), (_, csz, ksz, gsz, seed) in zip(got, records, cases):
            self.assertIsNone(shader_split.check('g.bin', st, ctl, code))
            self.assertEqual(code[:4], shader_split.GFX_MAGIC)
            self.assertEqual(code[0x30:0x30 + csz], region[:csz])                  # the code at +0x30
            c_end = (csz + 0x12f) & ~0xff
            self.assertEqual(code[c_end:c_end + ksz], region[csz:csz + ksz])       # its constants on the next 256-byte boundary
            self.assertEqual(struct.unpack_from('<II', ctl, 0x6f8), (csz, ksz))
            self.assertEqual(ctl[0x7d0:0x7e8], bytes([seed + 1]) * 0x18)           # GLSLC's compile information from the metadata
            if gsz:
                o = c_end + ((ksz + 0xff) & ~0xff) + 0x30
                self.assertEqual(code[o:o + gsz], region[csz + ksz:])              # the second code 0x30 into the block after the constants
                self.assertEqual(struct.unpack_from('<I', ctl, 0x758)[0], gsz)

    def test_compact_lookalikes_are_skipped(self):
        def rec(sph, csz, ksz=0x40):
            meta = bytearray(0x40); struct.pack_into('<III', meta, 0, 1, csz, ksz)
            return struct.pack('<III', csz + ksz, 0x40, 0) + struct.pack('<I', sph) + bytes(csz - 4 + ksz) + bytes(meta)
        self.assertEqual(shader_split.ue3_shaders(rec(0x0061 | 0xf << 10, 0x1d0), 'max.xxx'), [])   # no graphics stage in the SPH
        self.assertEqual(shader_split.ue3_shaders(rec(0x1462, 0x200), 'tunnel.xxx'), [])            # a code length GLSLC never writes: +0x704 would not match the code area
        self.assertEqual(shader_split.ue3_shaders(rec(0x1462, 0x1d0)[:-1], 'cut.xxx'), [])           # metadata past the end

    def test_key_skip_masks_the_compile_information(self):
        (c1, k1), (_, k2) = shader(1), shader(2)
        k2 = k1[:0x7d0] + k2[0x7d0:0x7e8] + k1[0x7e8:]   # the same section but its compile information
        shader_split.KEY_SKIP = (0, 0)
        self.assertNotEqual(shader_split.skey(k1, c1), shader_split.skey(k2, c1))
        self.assertFalse(shader_split.same_shader((k1, c1), (k2, c1)))
        shader_split.KEY_SKIP = (0x7d0, 0x18)
        self.assertEqual(shader_split.skey(k1, c1), shader_split.skey(k2, c1))
        self.assertTrue(shader_split.same_shader((k1, c1), (k2, c1)))
        self.assertEqual(shader_split.skey(k1, c1), shader_split.hashlib.sha256(k1[:0x7d0] + bytes(0x18) + k1[0x7e8:] + c1).hexdigest()[:16])


if __name__ == '__main__':
    unittest.main()
