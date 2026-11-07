"""BAHS control words that resemble binary descriptors must not become extra shaders. Synthetic NVN2 bytes only."""
import struct
import unittest

from dm.tools.tests.test_split_gzip import ss


class ControlDescriptors(unittest.TestCase):
    def test_control_words_are_not_descriptors_and_shared_controls_still_work(self):
        region = 0x400
        data = bytearray(region + 0x200)
        struct.pack_into('<II', data, 0x18, 0x200, region)
        for c in (0x100, 0x238):
            data[c:c + 4] = ss.CTL_MAGIC
            struct.pack_into('<I', data, c + 4, 0xc9)
            struct.pack_into('<I', data, c + 0x44, 0x100)
            data[c + 0x56] = 5
        # A real entry points to a control whose instruction/aligned/code sizes also look like an entry.
        struct.pack_into('<IIIi', data, 0x40, 0, 0, 0xe0, 0x100 - 0x40)
        struct.pack_into('<IIIi', data, 0x138, 0x100, 0, 0x100, 0x100)
        # Later real entries can reuse either control; rejecting control metadata must not reject those.
        struct.pack_into('<IIIi', data, 0x340, 0x100, 0, 0xe0, 0x238 - 0x340)
        struct.pack_into('<IIIi', data, 0x380, 0, 0, 0xe0, 0x100 - 0x380)
        shaders = ss.bahs_shaders(bytes(data), 'synthetic')
        self.assertEqual([s[0] for s in shaders], ['synthetic#BAHS@0x40', 'synthetic#BAHS@0x340', 'synthetic#BAHS@0x380'])
        self.assertEqual([len(s[2]) for s in shaders], [0xe0] * 3)


if __name__ == '__main__':
    unittest.main()
