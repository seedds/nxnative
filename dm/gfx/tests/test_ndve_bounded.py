"""Video-enhancement shader archives: synthetic records, no game assets."""
from pathlib import Path
import contextlib
import io
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/fuzz'))
import fuzzlib
split = fuzzlib.import_shader_split()


def shader(seed=1):
    ctl, code = bytearray(0xe0), bytes([seed]) * 0x100
    struct.pack_into('<II', ctl, 0, 0x98761234, 0xc9)
    struct.pack_into('<III', ctl, 0x38, len(code), 0, len(code))
    struct.pack_into('<I', ctl, 0x44, len(code))
    ctl[0x56] = 5
    struct.pack_into('<3I', ctl, 0x58, 8, 8, 1)
    return bytes(ctl), code


def ndsr():
    c, k = shader(2)
    d = bytearray(0x400)
    struct.pack_into('<4s4I', d, 0, b'NDSR', 0x40100, 0, 4, 0)
    # Two ordinary control/code records; the separate NDSR odd-entry fix is #1388.
    struct.pack_into('<8I', d, 0x14, 0x100, len(c) << 4 | 6, 0x200, len(k) << 4 | 7,
                     0x100, len(c) << 4 | 6, 0x300, len(k) << 4 | 7)
    d[0x100:0x100 + len(c)] = c
    d[0x200:0x300], d[0x300:0x400] = k, shader(3)[1]
    return bytes(d)


def network():
    c, k = shader()
    off = 8 + 8 + 16
    return struct.pack('<8I', 1, 1, off, 4, off + 4, len(c), len(k), 0) + b'abcd' + c + k


def container():
    member = network()
    archive = member + struct.pack('<5I', 60, 1, 6, 0, len(member)) + struct.pack('<I4s', len(member), b'1ALR')
    pack = ndsr()
    return bytearray(struct.pack('<4s5I', b'NDVE', 0x0ff00005, 0, len(pack), len(archive), 1) + bytes(8) + pack + archive)


class NdveTests(unittest.TestCase):
    def rejected(self, d):
        with self.assertRaises(SystemExit), contextlib.redirect_stdout(io.StringIO()): split.ndve_shaders(d, '/video')

    def test_complete_container_preserves_control_code_keys_and_stages(self):
        out = split.ndve_shaders(container(), '/video')
        self.assertEqual([(st, c, k) for _, st, c, k in out], [('cs', *shader()), ('cs', *shader(2)), ('cs', *shader(3))])
        self.assertEqual([split.skey(c, k) for _, _, c, k in out], [split.skey(*shader(i)) for i in (1, 2, 3)])

    def test_container_scanner_discovers_the_format(self):
        out = []
        split.scan_blob(container(), '/arbitrary-name', out)
        self.assertEqual(len(out), 3)

    def test_loose_and_compressed_files_pass_the_file_prefilter(self):
        zstd = split.zstd_mod()
        with tempfile.TemporaryDirectory() as tmp:
            for payload in (container(), zstd.compress(container())):
                p = Path(tmp) / 'arbitrary.dat'
                p.write_bytes(payload)
                _, found, _, _ = split.scan_file((str(p), p.name))
                self.assertEqual(len(found), 3)

    def test_truncated_header_payload_and_footer(self):
        d = container()
        for end in (4, 20, 31, len(d) - 1): self.rejected(d[:end])

    def test_unsupported_version_and_parameter_count(self):
        for off, value in ((4, 6), (8, 1), (20, 0xffffffff)):
            d = container(); struct.pack_into('<I', d, off, value); self.rejected(d)

    def test_archive_footer_and_index_bounds(self):
        d = container(); d[-1] ^= 1; self.rejected(d)
        d = container(); struct.pack_into('<I', d, len(d) - 8, len(d)); self.rejected(d)

    def test_member_cannot_overlap_the_index(self):
        d = container(); struct.pack_into('<I', d, len(d) - 12, len(d)); self.rejected(d)

    def test_shader_cannot_overlap_weights_or_exceed_the_member(self):
        start = 32 + len(ndsr())
        for off, value in ((start + 16, 32), (start + 20, 0xffffffff), (start + 28, 1)):
            d = container(); struct.pack_into('<I', d, off, value); self.rejected(d)

    def test_control_code_length_disagreement(self):
        d = container(); start = 32 + len(ndsr()) + 36
        struct.pack_into('<I', d, start + 0x44, 0x200); self.rejected(d)


if __name__ == '__main__': unittest.main()
