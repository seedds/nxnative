"""NXSD v0x100 containers, from issue #743's published layout; all bytes are generated."""
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '010015100B514000'
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had


def program(stage, tag):
    code = bytearray(0x100 + tag * 8)
    code[:4] = ss.CS_MAGIC if stage == 'cs' else ss.GFX_MAGIC
    if stage != 'cs': struct.pack_into('<I', code, 0x30, (ss.STAGES.index(stage) + 1) << 10 | 1)
    if stage == 'ps': code[0x48] = 1   # generic input 0, requiring the separable-program vertex padding cover
    code[0x80] = tag
    ctl = bytearray(0x880); ctl[:4] = ss.CTL_MAGIC
    struct.pack_into('<I', ctl, 0x20, len(ctl)); struct.pack_into('<I', ctl, 0x704, len(code))
    return bytes(ctl), bytes(code)


def container(stages=('vs', 'ps', 'cs'), order=None):
    programs = [program(st, i + 1) for i, st in enumerate(stages)]
    table = 0x50   # the header points here; it need not immediately follow the header
    controls = table + 24 * len(programs); codes = controls + 0x880 * len(programs)
    d = bytearray(codes); d[:4] = b'NXSD'
    offsets = []
    for _, code in programs: offsets.append(len(d) - codes); d += code
    struct.pack_into('<7I', d, 4, 0x100, len(d), codes, 0, len(programs), 0x880 * len(programs), len(d) - codes)
    struct.pack_into('<3Q', d, 0x28, table, controls, codes)
    for i, (ctl, _) in enumerate(programs): d[controls + i * 0x880:controls + (i + 1) * 0x880] = ctl
    for i, index in enumerate(range(len(programs)) if order is None else order):
        struct.pack_into('<6I', d, table + i * 24, index * 0x880, 0, offsets[index], 0, 0, 0)
    return d, programs


class Nxsd(unittest.TestCase):
    def scan(self, data, name='shaders.binary'):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root, name); path.write_bytes(data)
            return ss.scan_file_in((str(path), name))

    def test_magic_dispatch_preserves_mixed_stages(self):
        d, programs = container()
        rel, keys, pairs, trees = self.scan(d)
        self.assertEqual(rel, 'shaders.binary')
        self.assertEqual(len(keys), len(programs))
        self.assertEqual([(st, ctl, code) for _, st, ctl, code in keys.values()],
                         [(st, *p) for st, p in zip(('vs', 'ps', 'cs'), programs)])
        self.assertEqual(list(keys), [ss.skey(*p) for p in programs])
        self.assertEqual(pairs, set()); self.assertEqual(trees, {})
        self.assertTrue(all('#NXSD@' in row[0] for row in keys.values()))
        cover_pairs, cover = ss.separable_pairs(keys)
        vertex, pixel, _ = list(keys)
        self.assertEqual(cover_pairs, {(vertex, pixel)}); self.assertEqual(cover, [pixel])

    def test_explicit_offsets_not_descriptor_order(self):
        d, programs = container(('vs', 'hs', 'ds', 'gs', 'ps', 'cs'), (5, 2, 0, 4, 1, 3))
        _, keys, pairs, _ = self.scan(d, 'no-extension')
        self.assertEqual(len(keys), len(programs))
        self.assertEqual([(st, ctl, code) for _, st, ctl, code in keys.values()],
                         [(('vs', 'hs', 'ds', 'gs', 'ps', 'cs')[i], *programs[i]) for i in (5, 2, 0, 4, 1, 3)])
        self.assertFalse(pairs)

    def test_unknown_bytes_do_not_become_nxsd_by_extension(self):
        self.assertEqual(self.scan(b'not a shader', 'fake.nxsd')[1], {})

    def reject(self, d):
        with self.assertRaisesRegex(SystemExit, r'\[RED\].*NXSD'):
            self.scan(d)

    def test_header_truncation(self):
        d, _ = container()
        for size in (4, 8, 0x1f, 0x28, 0x3f):
            with self.subTest(size=size): self.reject(d[:size])

    def test_invalid_header_fields(self):
        d, _ = container()
        for offset, fmt, value in ((4, 'I', 0x101), (8, 'I', len(d) - 1), (12, 'I', 0),
                                   (20, 'I', 0), (20, 'I', 0xffffffff), (24, 'I', 3 * 0x881),
                                   (28, 'I', 0), (40, 'Q', 0), (40, 'Q', 1 << 63),
                                   (48, 'Q', 0), (56, 'Q', len(d) + 1)):
            with self.subTest(offset=offset, value=value):
                bad = bytearray(d); struct.pack_into('<' + fmt, bad, offset, value); self.reject(bad)

    def test_control_offsets_and_duplicate_descriptors(self):
        d, _ = container()
        for offset in (1, 0x880 * 3, 0xffffffff, 0x880):
            with self.subTest(offset=offset):
                bad = bytearray(d); struct.pack_into('<I', bad, 0x50, offset); self.reject(bad)

    def test_code_ranges_must_partition_the_region(self):
        d, programs = container(); n = len(programs[0][1])
        for offset in (1, n - 8, n + 8, 0xffffffff):
            with self.subTest(offset=offset):
                bad = bytearray(d); struct.pack_into('<I', bad, 0x50 + 24 + 8, offset); self.reject(bad)
        # Point the second descriptor at another valid complete code area: magic alone must not accept overlap.
        bad = bytearray(d); struct.pack_into('<I', bad, 0x50 + 24 + 8, 0)
        ctl = struct.unpack_from('<Q', bad, 48)[0]
        struct.pack_into('<I', bad, ctl + 0x880 + 0x704, n); self.reject(bad)

    def test_control_and_stage_validation(self):
        d, _ = container(); ctl, code = struct.unpack_from('<QQ', d, 48)
        for offset, value in ((ctl, 0), (ctl + 0x704, len(d)),
                              (code, 0), (code + 0x30, 15 << 10)):
            with self.subTest(offset=offset):
                bad = bytearray(d); struct.pack_into('<I', bad, offset, value)
                self.reject(bad)

    def test_complete_code_headers_required_before_pairing(self):
        for stage, size in (('ps', 0x34), ('vs', 0x7f), ('cs', 0x2f)):
            with self.subTest(stage=stage, size=size):
                d, programs = container(('vs', stage)); ctl, code = struct.unpack_from('<QQ', d, 48)
                n = len(programs[0][1]) + size; d = d[:code + n]
                struct.pack_into('<I', d, 8, len(d)); struct.pack_into('<I', d, 28, n)
                struct.pack_into('<I', d, ctl + 0x880 + 0x704, size)
                self.reject(d)   # magic + a stage word alone must not reach separable_pairs with an incomplete SPH

    def test_truncated_code_and_unclaimed_tail(self):
        d, _ = container(); self.reject(d[:-1])
        d += bytes(8); struct.pack_into('<I', d, 8, len(d))
        struct.pack_into('<I', d, 28, struct.unpack_from('<I', d, 28)[0] + 8)
        self.reject(d)


if __name__ == '__main__': unittest.main()
