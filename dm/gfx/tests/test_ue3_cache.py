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


def compact_record(ins0, data=b'', ins1=b'', *, x0=0x11, f710=0, b14=0, b15=0, b16=1, f724=0, x1=0x22, f794=0x40, compile_info=b'', meta_len=None):
    """A compact UE3 shader record: {u32 region_len, u32 meta_len (< 0x880), u32 0, region (ins0 || data || ins1), meta}."""
    region = ins0 + data + ins1
    meta = bytearray(max(0x28, 0x28 + len(compile_info), meta_len or 0))
    struct.pack_into('<5I', meta, 0x00, 1, len(ins0), len(data), x0, f710)
    meta[0x14], meta[0x15], meta[0x16] = b14, b15, b16
    struct.pack_into('<4I', meta, 0x18, f724, len(ins1), x1, f794)
    if compile_info:
        meta[0x28:0x28 + len(compile_info)] = compile_info
    if meta_len is not None:
        meta = meta[:meta_len]
    return struct.pack('<III', len(region), len(meta), 0) + region + bytes(meta)


class Ue3CompactRecordTests(unittest.TestCase):
    """UE3 compact records without stored NVN control sections (single-module, inline two-module VertexA+VertexB, multi-block c1 constants, metadata fields, lookalikes, and the CTL_MAGIC gate)."""
    # Maxwell SPH word 0: version 3 (0x60) | VTG (1) | stage 0 (VertexA) or stage 1 (VertexB), or PS (2) | stage 5.
    vs_a = struct.pack('<I', 0x0061) + bytes(0x6c)   # 0x70 bytes (0x30 + 0x70 = 0xa0 <= 0x100)
    vs_b = struct.pack('<I', 0x0461) + bytes(0x6c)   # 0x70 bytes
    ps = struct.pack('<I', 0x1462) + bytes(0x6c)     # 0x70 bytes
    c1_small = b'\x5a' * 0x40
    c1_large = b'\x3c' * 0x140   # > 0x100 bytes: requires 256-byte rounding (0x200), not a fixed 0x100 block

    def test_two_module_vertex_record(self):
        rec = compact_record(self.vs_a, self.c1_small, self.vs_b, x0=0x11, f710=0x33, b14=1, b15=0, b16=1, f724=0x44, x1=0x22, f794=0x40)
        got = shader_split.ue3_shaders(rec, 'gs.bin')
        self.assertEqual([(w, st) for w, st, _, _ in got], [('gs.bin#UE3@0', 'vs')])
        ctl, code = got[0][2], got[0][3]
        self.assertEqual(list(struct.unpack_from('<5I', ctl, 0x6f8)), [len(self.vs_a), len(self.c1_small), 0x100, 0x300, 0x30])
        self.assertEqual(list(struct.unpack_from('<6I', ctl, 0x748)), [0x30, len(self.vs_a), 0x11, 0x230, len(self.vs_b), 0x22])
        self.assertEqual([struct.unpack_from('<I', ctl, o)[0] for o in (0x70c, 0x710, 0x714, 0x718, 0x724, 0x734, 0x794)], [0x11, 0x33, 1, 0, 0x44, 1, 0x40])
        self.assertEqual(len(code), 0x300)
        self.assertEqual(code[:4], shader_split.GFX_MAGIC)
        self.assertEqual(code[0x30:0x30 + len(self.vs_a)], self.vs_a)
        self.assertEqual(code[0x100:0x100 + len(self.c1_small)], self.c1_small)
        self.assertEqual(code[0x200:0x230], bytes(0x30))
        self.assertEqual(code[0x230:0x230 + len(self.vs_b)], self.vs_b)
        self.assertEqual(code[0x230 + len(self.vs_b):], bytes(0x300 - 0x230 - len(self.vs_b)))

    def test_single_module_large_constants_and_full_metadata(self):
        info = bytes(range(0x18))
        rec = compact_record(self.ps, self.c1_large, x0=0x18, f710=0x04, b14=2, b15=1, b16=3, f724=0x55, f794=0x180, compile_info=info, meta_len=0x40)
        got = shader_split.ue3_shaders(rec, 'plants.xxx')
        self.assertEqual([(w, st) for w, st, _, _ in got], [('plants.xxx#UE3@0', 'ps')])
        ctl, code = got[0][2], got[0][3]
        self.assertEqual(list(struct.unpack_from('<4I', ctl, 0x6f8)), [len(self.ps), len(self.c1_large), 0x100, 0x300])
        self.assertEqual(ctl[0x748:0x760], bytes(0x18))
        self.assertEqual([struct.unpack_from('<I', ctl, o)[0] for o in (0x70c, 0x710, 0x714, 0x718, 0x724, 0x734, 0x794)], [0x18, 0x04, 2, 0x10000, 0x55, 3, 0x180])
        self.assertEqual(ctl[0x7d0:0x7e8], info)
        self.assertEqual(len(code), 0x300)
        self.assertEqual(code[0x100:0x100 + len(self.c1_large)], self.c1_large)
        self.assertEqual(code[0x100 + len(self.c1_large):], bytes(0x300 - 0x100 - len(self.c1_large)))

    def test_two_module_with_large_constants(self):
        rec = compact_record(self.vs_a, self.c1_large, self.vs_b, x0=0x10, x1=0x20)
        got = shader_split.ue3_shaders(rec, 'bigc1.xxx')
        self.assertEqual(len(got), 1)
        ctl, code = got[0][2], got[0][3]
        self.assertEqual(list(struct.unpack_from('<4I', ctl, 0x6f8)), [len(self.vs_a), len(self.c1_large), 0x100, 0x400])
        self.assertEqual(list(struct.unpack_from('<6I', ctl, 0x748)), [0x30, len(self.vs_a), 0x10, 0x330, len(self.vs_b), 0x20])
        self.assertEqual(code[0x330:0x330 + len(self.vs_b)], self.vs_b)

    def test_short_metadata_zero_pads_tail_fields(self):
        rec = compact_record(self.ps, self.c1_small, meta_len=0x20)
        got = shader_split.ue3_shaders(rec, 'short.bin')
        self.assertEqual(len(got), 1)
        ctl = got[0][2]
        self.assertEqual(struct.unpack_from('<I', ctl, 0x794)[0], 0)
        self.assertEqual(ctl[0x7d0:0x7e8], bytes(0x18))

    def test_rejects_lookalikes_and_misaligned_code_lengths(self):
        # Bad SPH stage (stage nibble 7 is not a Maxwell graphics stage)
        bad_sph = compact_record(struct.pack('<I', (7 << 10) | 0x61) + bytes(0x6c), self.c1_small)
        self.assertEqual(shader_split.ue3_shaders(bad_sph, 'bad_sph.xxx'), [])
        # Misaligned csz (0xe0: a256(0xe0) = 0x100 != a256(0x30 + 0xe0) = 0x200)
        misaligned = compact_record(struct.pack('<I', 0x1462) + bytes(0xdc), self.c1_small)
        self.assertEqual(shader_split.ue3_shaders(misaligned, 'misaligned.xxx'), [])
        # Bad second-module SPH (pixel shader in VertexB slot)
        bad_second = compact_record(self.vs_a, self.c1_small, self.ps)
        self.assertEqual(shader_split.ue3_shaders(bad_second, 'bad_second.xxx'), [])
        # Tail lengths do not sum to region length
        bad_len = bytearray(compact_record(self.vs_a, self.c1_small, self.vs_b))
        struct.pack_into('<I', bad_len, 12 + len(self.vs_a) + len(self.c1_small) + len(self.vs_b) + 0x1c, len(self.vs_b) + 4)
        self.assertEqual(shader_split.ue3_shaders(bytes(bad_len), 'bad_len.xxx'), [])

    def test_stored_control_section_gates_compact_scan(self):
        code, ctl = shader(1)
        mixed = record(code, ctl) + compact_record(self.vs_a, self.c1_small, self.vs_b)
        got = shader_split.ue3_shaders(mixed, 'mixed.bin')
        self.assertEqual([(st, c, k) for _, st, c, k in got], [('ps', ctl, code)])


if __name__ == '__main__':
    unittest.main()
