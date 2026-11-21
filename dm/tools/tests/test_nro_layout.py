"""A 4 KB NRO must execute code and write its globals on a 16 KB Mac page."""
import ctypes
import mmap
import os
from pathlib import Path
import platform
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import nro_layout
import nso
from nso_asm import ADRP


def module(data=0x2000):
    img = bytearray(data + 0x1000)
    img[0x10:0x14] = b'NRO0'
    struct.pack_into('<I', img, 4, 0x1000)
    struct.pack_into('<I', img, 0x18, len(img))
    struct.pack_into('<6I', img, 0x20, 0, 0x1000, 0x1000, data - 0x1000, data, 0x1000)
    struct.pack_into('<I', img, 0x38, 0x1000)
    img[0x1000:0x1004] = b'MOD0'
    struct.pack_into('<6i', img, 0x1004, 0x40, data, data + 0x1000, 0, 0, 0)
    # A relocated pointer to a counter, followed by native code that increments it.
    tags = ((7, 0x1200), (8, 24), (9, 24), (6, 0x1300), (5, 0x1400), (4, 0x1500), (3, data))
    for i, tag in enumerate(tags): struct.pack_into('<QQ', img, 0x1040 + 16 * i, *tag)
    struct.pack_into('<QQq', img, 0x1200, data, 1027, data + 0x100)
    struct.pack_into('<IBBHQQ', img, 0x1318, 0, 0x11, 0, 1, data + 0x100, 4)
    struct.pack_into('<II', img, 0x1500, 1, 2)
    struct.pack_into('<I', img, data + 0x100, 41)
    code = (ADRP(1, 0x80, data), 0xf9400022, 0xb9400040, 0x11000400, 0xb9000040, 0xd65f03c0)
    for i, w in enumerate(code): struct.pack_into('<I', img, 0x80 + 4 * i, w)
    return nso.load(img)


class NroLayout(unittest.TestCase):
    def test_addresses_and_original_identity(self):
        original = module(); before = bytes(original.image)
        patched = nro_layout.separate_data(original); b = patched.image
        self.assertEqual(bytes(original.image), before)
        self.assertEqual(patched.segments, ((0, 0x1000), (0x1000, 0x1000), (0x4000, 0x1000)))
        self.assertEqual(struct.unpack_from('<QQq', b, 0x1200), (0x4000, 1027, 0x4100))
        self.assertEqual(struct.unpack_from('<Q', b, 0x1320)[0], 0x4100)
        self.assertEqual(b[0x4000:], before[0x2000:])
        self.assertEqual((patched.bss, patched.build_id), (original.bss, original.build_id))
        self.assertEqual(struct.unpack_from('<I', b, 0x80)[0], ADRP(1, 0x80, 0x4000))

    def test_separated_module_unchanged(self):
        m = module(0x4000)
        self.assertIs(nro_layout.separate_data(m), m)

    def test_non_nro_unchanged(self):
        m = module(); m.image[0x10:0x14] = bytes(4)
        self.assertIs(nro_layout.separate_data(m), m)

    def test_bss_symbols_absolute_symbols_and_header_offsets(self):
        m = module(); b = m.image
        struct.pack_into('<Q', b, 0x1320, 0x3ff0)   # a bss symbol
        struct.pack_into('<I', b, 0x60, 0x2100)     # dso handle
        r = nro_layout.separate_data(m).image
        self.assertEqual(struct.unpack_from('<Q', r, 0x1320)[0], 0x5ff0)
        self.assertEqual(struct.unpack_from('<I', r, 0x60)[0], 0x4100)
        struct.pack_into('<H', b, 0x131e, 0xfff1)   # SHN_ABS values are constants
        self.assertEqual(struct.unpack_from('<Q', nro_layout.separate_data(m).image, 0x1320)[0], 0x3ff0)

    def test_adr_and_literal_reach_new_data(self):
        m = module(); b = m.image
        struct.pack_into('<I', b, 0x80, 0x10000000 | ((0x2100 - 0x80) >> 2 << 5))
        struct.pack_into('<I', b, 0x84, 0x58000000 | ((0x2100 - 0x84) // 4 << 5))
        r = nro_layout.separate_data(m).image
        self.assertEqual(struct.unpack_from('<I', r, 0x80)[0], 0x10000000 | ((0x4100 - 0x80) >> 2 << 5))
        self.assertEqual(struct.unpack_from('<I', r, 0x84)[0], 0x58000000 | ((0x4100 - 0x84) // 4 << 5))

    def test_relr_explicit_and_bitmap_addends(self):
        m = module(); b = m.image
        for i, tag in enumerate(((36, 0x1600), (35, 16), (37, 8))):
            struct.pack_into('<QQ', b, 0x10b0 + i * 16, *tag)
        struct.pack_into('<QQ', b, 0x1600, 0x2020, 3)
        struct.pack_into('<QQ', b, 0x2020, 0x2100, 0x80)
        r = nro_layout.separate_data(m).image
        self.assertEqual(struct.unpack_from('<QQ', r, 0x1600), (0x4020, 3))
        self.assertEqual(struct.unpack_from('<QQ', r, 0x4020), (0x4100, 0x80))

    def test_unsupported_relocation_does_not_mutate_input(self):
        m = module(); struct.pack_into('<QQ', m.image, 0x10b0, 17, 0x1600)
        before = bytes(m.image)
        with self.assertRaisesRegex(nso.Error, 'REL relocations'):
            nro_layout.separate_data(m)
        self.assertEqual(bytes(m.image), before)

    @unittest.skipUnless(sys.platform == 'darwin' and platform.machine() == 'arm64', 'native Apple page protections')
    def test_native_relocation_and_global_write(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'module.bin'
            m = nro_layout.separate_data(module()); path.write_bytes(m.image + bytes(m.bss))
            run = subprocess.run([sys.executable, __file__, '--execute', str(path)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--execute':
        raw = Path(sys.argv[2]).read_bytes()
        mem = mmap.mmap(-1, len(raw), prot=mmap.PROT_READ | mmap.PROT_WRITE)
        mem[:] = raw
        base = ctypes.addressof(ctypes.c_char.from_buffer(mem))
        # Apply the actual ELF RELATIVE record, then seal text as the Mac loader does.
        target, info, addend = struct.unpack_from('<QQq', raw, 0x1200)
        ctypes.c_uint64.from_address(base + target).value = base + addend
        libc = ctypes.CDLL(None)
        libc.sys_icache_invalidate(ctypes.c_void_p(base), ctypes.c_size_t(0x1000))
        assert libc.mprotect(ctypes.c_void_p(base), ctypes.c_size_t(0x4000), 5) == 0
        assert ctypes.CFUNCTYPE(ctypes.c_int)(base + 0x80)() == 42
        assert ctypes.c_uint32.from_address(base + addend).value == 42
        os._exit(0)
    unittest.main()
