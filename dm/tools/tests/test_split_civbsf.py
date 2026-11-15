"""CIVBSF libraries reach the split through their indexed NVN payloads. Synthetic bytes only."""
import contextlib
import io
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'fuzz'))
import fuzz_shader_split as fz

ss = fz.ss


def library(stages):
    """Indexed single stages; the first pointer is unrelated metadata, the second a descriptor. Unaligned data includes control trailing bytes and nonzero payload flags."""
    data = bytearray(0x90 + len(stages) * 0x50); data[:8] = ss.CIVBSF
    struct.pack_into('<Q', data, 0x20, 0x90)
    struct.pack_into('<I', data, 0x68, len(stages))
    for i, (ctl, code) in enumerate(stages):
        entry = 0x90 + 0x50 * i; descriptor = len(data)
        data += bytes(16) + b'x'
        offset = len(data); extra = bytes(0x80)
        payload = struct.pack('<4I', 0xff00ff00, len(code), len(ctl) + len(extra), 0x60000) + code + ctl + extra
        struct.pack_into('<QQ', data, entry, 0xffffffffffffffff, descriptor)
        struct.pack_into('<QQ', data, descriptor, offset, len(payload))
        data += payload
    struct.pack_into('<I', data, 0xc, len(data))
    return bytes(data)


class CivbsfShaders(unittest.TestCase):
    def scan(self, data):
        with tempfile.TemporaryDirectory() as root, contextlib.redirect_stdout(io.StringIO()) as log:
            source = Path(root) / 'opaque.bin'; source.write_bytes(data)
            result = ss.scan_file((str(source), 'opaque.bin'))
        return result, log.getvalue()

    def rejected(self, data, reason):
        with self.assertRaisesRegex(SystemExit, r'\[RED\] .*CIVBSF.*' + reason):
            ss.civbsf_shaders(data, '/fixture')

    def test_indexed_vertex_pixel_and_compute_keep_original_keys(self):
        control = bytearray(fz.VS[0]); struct.pack_into('<3I', control, 0x748, 8, 1, 1)
        compute = (bytes(control), ss.CS_MAGIC + fz.VS[1][4:])
        stages = [fz.VS, fz.PS, compute]
        found = ss.civbsf_shaders(library(stages), '/fixture')
        self.assertEqual([s for _, s, _, _ in found], ['vs', 'ps', 'cs'])
        self.assertEqual([ss.skey(c, k) for _, _, c, k in found], [ss.skey(c, k) for c, k in stages])
        self.assertEqual([len(c) for _, _, c, _ in found], [0x900] * 3)
        (rel, keys, pairs, extra), log = self.scan(library(stages))
        self.assertEqual((rel, pairs, extra, log), ('opaque.bin', set(), {}, ''))
        self.assertEqual(set(keys), {ss.skey(c, k) for c, k in stages})

    def test_single_stage_cover_pads_vertices_without_inventing_program_pairs(self):
        ctl, code = fz.PS; code = bytearray(code)
        struct.pack_into('<I', code, 0x48, 1)   # pixel shader reads generic location 0
        pixel = (ctl, bytes(code)); found = ss.civbsf_shaders(library([fz.VS, pixel]), '/fixture')
        self.assertEqual(ss.var_pairs(found), set())
        pairs, cover = ss.separable_pairs(ss.uniq_keys(found))
        self.assertEqual((pairs, cover), ({(ss.skey(*fz.VS), ss.skey(*pixel))}, [ss.skey(*pixel)]))

    def test_nested_archive_uses_the_same_reader(self):
        found = []; ss.scan_blob(fz.sarc([(b'opaque.bin', library([fz.VS]))]), '/pack', found)
        self.assertEqual([ss.skey(c, k) for _, _, c, k in found], [ss.skey(*fz.VS)])

    def test_header_and_table_are_bounded(self):
        data = library([fz.VS])
        self.rejected(data[:0x80], 'outside')
        self.rejected(data[:-1], 'file size')
        for offset, fmt, value in ((0x20, '<Q', 0), (0x20, '<Q', len(data)), (0x68, '<I', 0xffffffff)):
            changed = bytearray(data); struct.pack_into(fmt, changed, offset, value)
            with self.subTest(offset=offset, value=value): self.rejected(changed, 'header|outside')

    def test_descriptor_and_payload_are_bounded(self):
        data = library([fz.VS]); descriptor, = struct.unpack_from('<Q', data, 0x98)
        for offset, value in ((0x98, len(data) - 8), (descriptor, len(data) - 8), (descriptor + 8, 0xffffffffffffffff)):
            changed = bytearray(data); struct.pack_into('<Q', changed, offset, value)
            with self.subTest(offset=offset): self.rejected(changed, 'outside')

    def test_payload_lengths_tag_stage_and_control_are_checked(self):
        data = library([fz.VS]); descriptor, = struct.unpack_from('<Q', data, 0x98); offset, = struct.unpack_from('<Q', data, descriptor)
        for at, value, reason in ((offset, 0, 'tag'), (offset + 4, 0xffffffff, 'lengths'),
                                  (offset + 16, 0, 'stage'), (offset + 16 + len(fz.VS[1]) + 0x704, 1, 'code area length')):
            changed = bytearray(data); struct.pack_into('<I', changed, at, value)
            with self.subTest(reason=reason): self.rejected(changed, reason)
        changed = bytearray(data); struct.pack_into('<Q', changed, descriptor + 8, 8)
        self.rejected(changed, 'outside')

    def test_damaged_file_is_reported_as_unreadable(self):
        (rel, keys, pairs, extra), log = self.scan(library([fz.VS])[:-1])
        self.assertEqual((rel, keys, pairs), ('opaque.bin', {}, set()))
        self.assertEqual(extra[0], 'unreadable')
        self.assertIn('[RED] /opaque.bin CIVBSF: file size', log)


if __name__ == '__main__': unittest.main()
