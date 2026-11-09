"""Tests for GIANTS Engine (.sharc) shader archive reader."""
from pathlib import Path
import lzma
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
os.environ.setdefault('DM_GAME', '010015100B514000')
import shader_split as split


def make_glslc_shader(stage):
    ctl = bytearray(0x880)
    code = bytearray(0x100)
    ctl[:4] = split.CTL_MAGIC
    struct.pack_into('<I', ctl, 0x20, len(ctl))
    struct.pack_into('<I', ctl, 0x704, len(code))
    code[:4] = split.CS_MAGIC if stage == 'cs' else split.GFX_MAGIC
    if stage != 'cs':
        struct.pack_into('<I', code, 0x30, (split.STAGES.index(stage) + 1) << 10 | 1)
    if stage == 'ps':
        struct.pack_into('<I', code, 0x48, 1)

    # Prefix with GLSLC header
    # 0x00: magic 0x19866891
    # 0x44: whole length
    # ctl starts at 0x90
    glslc = bytearray(0x90) + ctl + code
    struct.pack_into('<I', glslc, 0x00, 0x19866891)
    struct.pack_into('<I', glslc, 0x44, len(glslc))
    struct.pack_into('<I', glslc, 0x4c, 1)
    return bytes(glslc)


def lzma_compress_raw(data):
    alone = lzma.compress(data, format=lzma.FORMAT_ALONE)
    return alone[:5] + alone[13:]


def build_sharc(shaders):
    """Build a minimal valid .sharc container holding the given shaders."""
    # 1 table1 entry
    t1_payload = b'T1METADATA'
    t1_entry = struct.pack('<IHH', len(t1_payload), 0, 0) + bytes(16)

    # Table 2 entries and data
    t2_entries = []
    t2_data = bytearray()
    for sh in shaders:
        lzma_data = lzma_compress_raw(sh)
        sz = len(lzma_data)
        w1, w2, w7 = 0, 0, 1
        t2_entries.append(struct.pack('<8I', sz, w1, w2, 0, 0, 0, 0, w7))
        t2_data += lzma_data
        # string entry for w7=1: [tag byte, len byte, string bytes]
        dummy_str = b'Technique0'
        t2_data += bytes([0, len(dummy_str)]) + dummy_str

    c1 = 1
    c2 = len(shaders)
    c3 = 1
    t3_entry = bytes(20)

    h_size = 0x2c
    header = struct.pack('<11I', 0x53484152, 11, 0, 0, h_size, c1, c2, c3, 0, 0, 0)
    return header + t1_entry + b''.join(t2_entries) + t3_entry + t1_payload + bytes(t2_data)


class SharcTests(unittest.TestCase):
    def test_sharc_shaders_extraction(self):
        vs = make_glslc_shader('vs')
        ps = make_glslc_shader('ps')
        cs = make_glslc_shader('cs')

        sharc_data = build_sharc([vs, ps, cs])
        found = split.sharc_shaders(sharc_data, '/test.sharc')

        self.assertEqual(len(found), 3)
        self.assertEqual([stage for _, stage, _, _ in found], ['vs', 'ps', 'cs'])

    def test_sharc_scan_file(self):
        vs = make_glslc_shader('vs')
        ps = make_glslc_shader('ps')
        sharc_data = build_sharc([vs, ps])

        with tempfile.TemporaryDirectory() as tmpdir:
            file_path = os.path.join(tmpdir, 'shader_switch.sharc')
            with open(file_path, 'wb') as f:
                f.write(sharc_data)

            rel, uniq, pairs, _ = split.scan_file((file_path, 'shader_switch.sharc'))
            self.assertEqual(rel, 'shader_switch.sharc')
            self.assertEqual(len(uniq), 2)

    def test_invalid_magic_or_version(self):
        bad_magic = b'NOPE' + bytes(40)
        self.assertEqual(split.sharc_shaders(bad_magic, '/bad'), [])

        bad_ver = struct.pack('<11I', 0x53484152, 99, 0, 0, 0x2c, 0, 0, 0, 0, 0, 0)
        with self.assertRaises(SystemExit):
            split.sharc_shaders(bad_ver, '/bad_ver')


if __name__ == '__main__':
    unittest.main()
