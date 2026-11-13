"""Needle's embedded NVN shader records, without game data."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split as split


def shader(stage):
    ctl, code = bytearray(0x850), bytearray(0x100)
    ctl[:4] = split.CTL_MAGIC
    struct.pack_into('<I', ctl, 0x20, len(ctl))
    struct.pack_into('<I', ctl, 0x704, len(code))
    if stage == 'cs':
        struct.pack_into('<3I', ctl, 0x748, 8, 4, 1)
    else:
        code[:4] = split.GFX_MAGIC
        struct.pack_into('<I', code, 0x30, (1 if stage == 'vs' else 5) << 10 | 1)
        if stage == 'ps': struct.pack_into('<I', code, 0x48, 1)   # pixel shader reads generic location 0
    header = bytearray(0x90)
    struct.pack_into('<4I', header, 0x30, 0x90, 0x90 + len(ctl), len(code), len(ctl))
    return bytes(header + ctl + code), bytes(ctl), bytes(code)


def shader2(stage):
    # An NVN2 record (Sonic Frontiers Definitive Edition): the descriptor 0x98 bytes before a 0xd8-byte control section, whose own length is at +0x30, the code length at +0x44
    # and the stage at +0x56; graphics code starts with its SPH, without a code header.
    ctl, code = bytearray(0xd8), bytearray(0x400)
    ctl[:8] = split.CTL_MAGIC + b'\xc9\0\0\0'
    struct.pack_into('<I', ctl, 0x30, len(ctl))
    struct.pack_into('<I', ctl, 0x44, len(code))
    if stage == 'cs': ctl[0x56] = 5
    else: struct.pack_into('<I', code, 0, (1 if stage == 'vs' else 5) << 10 | 1)
    header = bytearray(0x98)
    struct.pack_into('<4I', header, 0x30, 0x98, 0x98 + len(ctl), len(code), len(ctl))
    return bytes(header + ctl + code), bytes(ctl), bytes(code)


def container(*stages, record=shader):
    # Reflection/variant metadata can precede and separate the NVN records.
    d = bytearray(b'HNSHV002' + bytes(24) + b'variant metadata')
    for stage in stages:
        d += record(stage)[0] + bytes(36)
    struct.pack_into('>I', d, 8, len(d))
    return bytes(d)


def pacx403(block, types=b'VertexShader\0PixelShader\0', deps=False):
    # Hedgehog Engine 2's archive (Sonic Frontiers Definitive Edition's raw/NeedleShader.pac): the root and one block, each in LZ4 chunks of 64 KB.
    import lz4.block
    def chunked(b):
        parts = [b[i:i + 0x10000] for i in range(0, len(b), 0x10000)]
        return [(lz4.block.compress(x, store_size=False), len(x)) for x in parts]
    bchunks = chunked(block)
    nodes = bytearray(0x10)
    tab = 0x30 + len(nodes)                                   # u64 count, u64 pointer, then the 0x20-byte entry
    ent, cks = tab + 0x10, tab + 0x30
    name = cks + 8 * len(bchunks)
    tail = b'NeedleShader.pac.000\0' + types
    root = bytearray(b'PACx402L' + bytes(8))
    struct.pack_into('<I', root, 0x0c, 0); root += bytes(0x30 - len(root))
    struct.pack_into('<I', root, 0x10, len(nodes)); root += nodes
    root += struct.pack('<QQ', 1, ent)
    root += struct.pack('<QIIIIQ', name, sum(len(c) for c, _ in bchunks), len(block), 0x50 + (0x18 if deps else 0), len(bchunks), cks)
    for c, u in bchunks: root += struct.pack('<II', len(c), u)
    root += tail
    rchunks = chunked(bytes(root))
    dep = 0x18 if deps else 0                                # a count, a pointer and one name pointer
    ct = 0x30 + dep
    body = bytearray(0x50 + dep)
    for c, _ in bchunks: body += c
    roff = len(body)
    for c, _ in rchunks: body += c
    body[:8] = b'PACx403L'
    struct.pack_into('<III', body, 0x10, roff, sum(len(c) for c, _ in rchunks), len(root))
    struct.pack_into('<II', body, 0x20, dep, 8 + 8 * len(rchunks))
    if deps: struct.pack_into('<QQQ', body, 0x30, 1, 0x40, 0)
    struct.pack_into('<I', body, ct, len(rchunks))
    for i, (c, u) in enumerate(rchunks): struct.pack_into('<II', body, ct + 4 + 8 * i, len(c), u)
    struct.pack_into('<I', body, 0x0c, len(body))
    return bytes(body)


class NeedleTests(unittest.TestCase):
    def test_pacx403_blocks(self):
        block = b'PACx402L' + bytes(0x28) + container('vs', 'ps', 'cs', record=shader2) + bytes(0x12000)   # two chunks
        found = []
        split.scan_blob(pacx403(block), 'NeedleShader.pac', found)
        self.assertEqual([s for _, s, _, _ in found], ['vs', 'ps', 'cs'])
        self.assertTrue(all(label.startswith('NeedleShader.pac/NeedleShader.pac.000#NEEDLE@') for label, *_ in found))
        # a dependency table before the root's chunk table moves it (CommonObject.pac); the result is the same
        found = []
        split.scan_blob(pacx403(block, deps=True), 'CommonObject.pac', found)
        self.assertEqual([s for _, s, _, _ in found], ['vs', 'ps', 'cs'])
        # an archive whose root names no shader type is not decompressed
        self.assertEqual(split.pacx4_blocks(pacx403(block, types=b'Texture\0'), 'stage.pac'), [])

    def test_embedded_variants_and_headerless_compute(self):
        data = b'prefix\0' + container('vs', 'ps', 'cs') + b'suffix'
        found = []
        split.scan_blob(data, 'main', found)
        self.assertEqual([s for _, s, _, _ in found], ['vs', 'ps', 'cs'])
        for (_, stage, ctl, code) in found:
            self.assertEqual((ctl, code), shader(stage)[1:])
        keys = split.uniq_keys(found)
        # These stages are independent programs; the main/GSC wrapper is not a pair.
        self.assertEqual(split.var_pairs(found), set())
        self.assertEqual(len(split.separable_pairs(keys)[0]), 1)
        self.assertTrue(all('#NEEDLE@' in label for label, *_ in found))

    def test_nvn2_records(self):
        data = b'prefix\0' + container('vs', 'ps', 'cs', record=shader2) + b'suffix'
        found = split.needle_shaders(data, 'main')
        self.assertEqual([s for _, s, _, _ in found], ['vs', 'ps', 'cs'])
        for (_, stage, ctl, code) in found:
            self.assertEqual((ctl, code), shader2(stage)[1:])
        # a small container holds an NVN2 record: the NVN size floor does not skip it
        self.assertEqual(len(split.needle_shaders(container('cs', record=shader2), 'small')), 1)

    def test_nvn2_control_length_must_match_descriptor(self):
        data = bytearray(container('vs', record=shader2))
        c = data.index(split.CTL_MAGIC)
        struct.pack_into('<I', data, c - 0x98 + 0x3c, 0xd0)
        with self.assertRaisesRegex(SystemExit, 'control length'):
            split.needle_shaders(data, 'bad control length')

    def test_container_bounds_reject_truncated_or_string_magic(self):
        self.assertEqual(split.needle_shaders(b'HNSHV002', 'short'), [])
        self.assertEqual(split.needle_shaders(container('vs')[:-1], 'truncated'), [])
        data = bytearray(container('vs'))
        struct.pack_into('>I', data, 8, 32)
        self.assertEqual(split.needle_shaders(data, 'short envelope'), [])

    def test_descriptor_cannot_borrow_code_from_next_record(self):
        data = bytearray(container('vs'))
        c = data.index(split.CTL_MAGIC)
        struct.pack_into('<I', data, c - 0x90 + 0x34, len(data))
        with self.assertRaisesRegex(SystemExit, 'outside its HNSHV002 container'):
            split.needle_shaders(data + container('ps'), 'bad code offset')

    def test_control_length_must_match_descriptor(self):
        data = bytearray(container('vs'))
        c = data.index(split.CTL_MAGIC)
        struct.pack_into('<I', data, c - 0x90 + 0x3c, 0x880)
        with self.assertRaisesRegex(SystemExit, 'control length'):
            split.needle_shaders(data, 'bad control length')


if __name__ == '__main__':
    unittest.main()
