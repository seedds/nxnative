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


def aa_record(ins0, data, ins1=b'', x=0x11, x1=0x22, a=0, b=1, y=0x40):
    """A Batman: Arkham Asylum record: {u8 GUID 16, u32 v1, u16 v2 (0x105), u32 v3, u32 v4 (the stored bytes' length), u32 0x28, u32 0, v4 bytes, 0x50-byte tail}. The v4 bytes are the first
    module's instruction bytes, its data section and the second module's instruction bytes (ins1, none by default): the second module is stored inline behind the first. The tail's +4, +8, +0xc,
    +0x14 (u16), +0x16 (u16), +0x1c, +0x20 and +0x24 are the fields the control section is built from (shader_split.aa_records)"""
    stored = ins0 + data + ins1
    tail = struct.pack('<IIII', 1, len(ins0), len(data), x) + bytes(4) + struct.pack('<HH', a, b) + bytes(4)
    tail += struct.pack('<III', len(ins1), x1, y)
    return bytes(16) + struct.pack('<IHII', 0, 0x105, 3, len(stored)) + struct.pack('<II', 0x28, 0) + stored + tail + bytes(0x50 - len(tail))


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

    def test_unis_resolves_independent_shared_buffer_offsets(self):
        code1, ctl1 = shader(1)
        code2, ctl2 = shader(2)
        header = shader_split.UE3_TAG + struct.pack('<HHI', 846, 108, 32)
        header += b'\x10\0\0\0UniShaderBuffer\0'
        blocks = ctl2 + ctl1, code1 + code2, b'reflection'
        sidecar = header + bytes(0x30) + b''.join(struct.pack('<I', len(b)) + b for b in blocks)
        buffers = shader_split.ue3_shared_buffers(sidecar, 'UniShaderBuffer-NX64.upk')

        def ref(code_off, ctl_off):
            return struct.pack('<I4s6I', 0x20, b'UNIS', 1, len(code1), code_off, len(ctl1), ctl_off, 0) + bytes(4)

        cache = b'BMSG' + ref(0, len(ctl2)) + ref(len(code1), 0) + ref(0, len(ctl2))
        got = shader_split.ue3_shared_shaders(cache, 'cache', *buffers)
        self.assertEqual([(st, ctl, code) for _, st, ctl, code in got], [('ps', ctl1, code1), ('ps', ctl2, code2)])
        with self.assertRaisesRegex(SystemExit, 'outside'):
            shader_split.ue3_shared_shaders(ref(2 * len(code1), 0), 'cache', *buffers)
        with self.assertRaisesRegex(SystemExit, 'reflection array'):
            shader_split.ue3_shared_buffers(sidecar[:-1], 'UniShaderBuffer-NX64.upk')

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


class ArkhamAsylumRecordTests(unittest.TestCase):
    """Batman: Arkham Asylum's records, built from synthetic bytes: a first module's instructions (0x20 bytes, SPH word 0 a vertex shader) and 0x40 bytes of data, with and without a second
    module's 0x40 instruction bytes stored inline behind the data. The control section the reader synthesises must name the code area's length at +0x704, list both modules at +0x748, and leave the
    second module's 0x30-byte header zeros; a record with no second module must come out exactly as the single-module path builds it"""
    ins0, data, ins1 = struct.pack('<I', 1) + bytes(0x1c), b'\x5a' * 0x40, b'\xa5' * 0x40

    def test_two_module_record(self):
        rec = aa_record(self.ins0, self.data, self.ins1, x=0x11, x1=0x22, a=0, b=1, y=0x40)
        got = shader_split.ue3_shaders(rec, 'gs.bin')
        self.assertEqual([(w, st) for w, st, _, _ in got], [('gs.bin#UE3@0', 'vs')])
        ctl, code = got[0][2], got[0][3]
        data_off, pair = 0x100, 0x230   # aa_data_off(0x20), then its 0x100-byte data section and the second module's 0x30-byte header
        self.assertEqual(struct.unpack_from('<I', ctl, 0x704)[0], 0x300)   # align(pair + 0x40)
        self.assertEqual(struct.unpack_from('<I', ctl, 0x704)[0], (pair + len(self.ins1) + 0xff) & ~0xff)
        self.assertEqual(list(struct.unpack_from('<4I', ctl, 0x6f8)), [len(self.ins0), len(self.data), data_off, 0x300])   # the scalars repeat the first triple
        self.assertEqual(list(struct.unpack_from('<6I', ctl, 0x748)), [0x30, len(self.ins0), 0x11, pair, len(self.ins1), 0x22])
        self.assertEqual([struct.unpack_from('<I', ctl, at)[0] for at in (0x70c, 0x714, 0x734, 0x794)], [0x11, 0, 1, 0x40])   # x, a, b, y
        self.assertEqual(len(code), 0x300)
        self.assertEqual(code[:4], shader_split.GFX_MAGIC)
        self.assertEqual(code[0x30:0x30 + len(self.ins0)], self.ins0)
        self.assertEqual(code[data_off:data_off + len(self.data)], self.data)
        self.assertEqual(code[pair - 0x30:pair], bytes(0x30))   # the second module's own header: no graphics magic of its own
        self.assertEqual(code[pair:pair + len(self.ins1)], self.ins1)
        self.assertEqual(code[pair + len(self.ins1):], bytes(0x300 - pair - len(self.ins1)))

    def test_a_record_without_a_second_module_is_the_single_module_path(self):
        rec = aa_record(self.ins0, self.data, x=0x11, x1=0x22, a=0, b=1, y=0x40)
        got = shader_split.ue3_shaders(rec, 'gs0.bin')
        self.assertEqual([(w, st) for w, st, _, _ in got], [('gs0.bin#UE3@0', 'vs')])
        ctl, code = got[0][2], got[0][3]
        self.assertEqual(ctl, shader_split.aa_ctl(len(self.ins0), len(self.data), 0x11, 0, 1, 0x40))   # byte for byte, no second module
        self.assertEqual(code, shader_split.aa_code(self.ins0 + self.data, len(self.ins0), len(self.data)))
        self.assertEqual(ctl, shader_split.aa_ctl(len(self.ins0), len(self.data), 0x11, 0, 1, 0x40, 0, 0))
        self.assertEqual(code, shader_split.aa_code(self.ins0 + self.data, len(self.ins0), len(self.data), 0))
        self.assertEqual(struct.unpack_from('<I', ctl, 0x704)[0], 0x200)   # the data section's end, not a second module's
        self.assertEqual(ctl[0x748:0x760], bytes(0x18))   # no pair table
        self.assertEqual(len(code), 0x200)
        self.assertEqual(code[0x100:0x140], self.data)
        self.assertEqual(code[0x140:], bytes(0xc0))

    def test_a_tail_that_does_not_add_up_is_not_a_record(self):
        rec = bytearray(aa_record(self.ins0, self.data, self.ins1))
        self.assertEqual(len(shader_split.ue3_shaders(bytes(rec), 'gs.bin')), 1)
        struct.pack_into('<I', rec, 38 + len(self.ins0) + len(self.data) + len(self.ins1) + 0x1c, len(self.ins1) + 4)   # the tail's second module is longer than v4 allows
        self.assertEqual(shader_split.ue3_shaders(bytes(rec), 'gs.bin'), [])

    def test_a_stored_control_section_keeps_the_records_it_had(self):
        code, ctl = shader(1)
        got = shader_split.ue3_shaders(record(code, ctl) + aa_record(self.ins0, self.data, self.ins1), 'mixed.bin')
        self.assertEqual([(st, c, k) for _, st, c, k in got], [('ps', ctl, code)])   # the gate: a document with a control section is read as before


if __name__ == '__main__':
    unittest.main()
