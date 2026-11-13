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
    # An NVN2 record (Sonic Frontiers Definitive Edition): a 0x98-byte descriptor, a 0xd8-byte control section (version 0xc9, its own length at +0x30, the code's at +0x44, a compute
    # stage at +0x56) and the code right after it, a graphics stage starting with its SPH.
    ctl, code = bytearray(0xd8), bytearray(0x200)
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


def lz4_chunks(b, stored=()):
    """b in 64 KB chunks: (bytes, stored length, length), LZ4 except the chunks numbered in stored"""
    import lz4.block
    out = []
    for i in range(0, max(len(b), 1), 0x10000):
        part = b[i:i + 0x10000]
        c = part if i // 0x10000 in stored else lz4.block.compress(part, store_size=False)
        out.append((c, len(c), len(part)))
    return out


def pacx403(blocks, types=b'VertexShader\0PixelShader\0', deps=0):
    """A PACx 403 archive: blocks are (name, PACx 402 bytes); with none, the root holds the resources (the types string stands in for its trees). deps: a dependency table's length"""
    data, entries = bytearray(), []
    for name, b in blocks:
        ch = lz4_chunks(b, stored={1})
        entries.append((name, sum(c for _, c, _ in ch), len(b), ch))
        data += b''.join(c for c, _, _ in ch)
    trees = bytearray(b'type tree\0' + types)
    trees += bytes(-len(trees) % 8)
    root = bytearray(b'PACx402L' + bytes(0x28)) + trees
    info_at = len(root)
    info = 0x10 + sum(0x20 + 8 * len(ch) for *_, ch in entries) if entries else 0
    tab = info_at + 0x10
    cks = tab + 0x20 * len(entries)
    names = cks + 8 * sum(len(ch) for *_, ch in entries)
    if entries: root += struct.pack('<QQ', len(entries), tab)
    off, k, nm = 0, cks, names
    for name, stored, ln, ch in entries:
        root += struct.pack('<QIIIIQ', nm, stored, ln, 0, len(ch), k)   # offset filled below
        k += 8 * len(ch); nm += len(name) + 1
    for *_, ch in entries:
        for _, c, u in ch: root += struct.pack('<II', c, u)
    for name, *_ in entries: root += name + b'\0'
    struct.pack_into('<II', root, 0x10, len(trees), info)
    ctab = (4 + 8 * -(-len(root) // 0x10000) + 7) & ~7   # the root's chunk count follows from its length
    head = bytearray(0x30 + deps + ctab)
    at = len(head)
    for i, (_, stored, *_) in enumerate(entries):   # the blocks follow the header, the root comes last
        struct.pack_into('<I', root, tab + 0x20 * i + 0x10, at); at += stored
    rch = lz4_chunks(bytes(root))
    head[:8] = b'PACx403L'
    struct.pack_into('<III', head, 0x10, at, sum(c for _, c, _ in rch), len(root))
    struct.pack_into('<II', head, 0x20, deps, ctab)
    struct.pack_into('<I', head, 0x30 + deps, len(rch))
    for i, (_, c, u) in enumerate(rch): struct.pack_into('<II', head, 0x34 + deps + 8 * i, c, u)
    return bytes(head + data + b''.join(c for c, _, _ in rch))


class NeedleTests(unittest.TestCase):
    def test_nvn2_records(self):
        data = b'prefix\0' + container('vs', 'ps', 'cs', record=shader2) + b'suffix'
        found = split.needle_shaders(data, 'main')
        self.assertEqual([s for _, s, _, _ in found], ['vs', 'ps', 'cs'])
        for (_, stage, ctl, code) in found:
            self.assertEqual((ctl, code), shader2(stage)[1:])
        # a container of one NVN2 record is smaller than one NVN record: it is still read
        self.assertEqual(len(split.needle_shaders(container('cs', record=shader2), 'small')), 1)

    def test_nvn2_control_length_must_match_descriptor(self):
        data = bytearray(container('vs', record=shader2))
        c = data.index(split.CTL_MAGIC)
        struct.pack_into('<I', data, c - 0x98 + 0x3c, 0xd0)
        with self.assertRaisesRegex(SystemExit, 'control length'):
            split.needle_shaders(data, 'bad control length')

    def test_pacx403_blocks(self):
        # two blocks over several chunks (one of them stored), the second with the Needle container; the archive's raw bytes hold no readable container
        a = b'PACx402L' + bytes(0x28) + bytes(0x18000)
        b = b'PACx402L' + bytes(0x28) + container('vs', 'ps', 'cs', record=shader2) + bytes(0x12000)
        for deps in (0, 0x18):   # a dependency table moves the root's chunk table
            with self.subTest(deps=deps):
                found = []
                split.scan_blob(pacx403([(b'shaders.000', a), (b'shaders.001', b)], deps=deps), '/raw/x.pac', found)
                self.assertEqual([s for _, s, _, _ in found], ['vs', 'ps', 'cs'])
                self.assertTrue(all(label.startswith('/raw/x.pac/shaders.001#NEEDLE@') for label, *_ in found))

    def test_pacx403_resources_in_the_root(self):
        found = []
        split.scan_blob(pacx403([], types=b'ComputeShader\0' + container('cs', record=shader2)), '/raw/y.pac', found)
        self.assertEqual([(label.split('#')[0], s) for label, s, _, _ in found], [('/raw/y.pac/root', 'cs')])

    def test_pacx403_without_shader_types_is_not_opened(self):
        d = pacx403([(b'stage.000', b'PACx402L' + bytes(0x28) + container('vs', record=shader2))], types=b'Texture\0Model\0')
        self.assertEqual(split.pacx4_parts(d, 'stage.pac'), [])

    def test_pacx403_cut_short(self):
        d = pacx403([(b'shaders.000', b'PACx402L' + bytes(0x28))])
        with self.assertRaisesRegex(SystemExit, 'lie outside'):
            split.pacx4_parts(d[:-1], 'cut.pac')   # the root's last chunk cut short

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
