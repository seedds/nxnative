"""shader_split: revision-4 Next Level Games archives (Mario Strikers: Battle League). zstd frames padded between them decompress as one stream; the shader table is read by
its four-entry groups; a program's code area and control section come from the base '.data' or, past the old section's length, from the patch body; BNSH containers are looked for in the data and the patch body of every archive. No game files needed."""
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '01007BB017812000'   # shader_split loads a profile at import; other tests expect no default game
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had


def zs(b): return subprocess.run(['zstd', '-q', '-c', '--no-check'], input=b, capture_output=True, check=True).stdout
def zs_check(b): return subprocess.run(['zstd', '-q', '-c', '--check'], input=b, capture_output=True, check=True).stdout


def ctl(code_len, tag):   # a control section: magic, its own length at +0x20, the code length at +0x704
    c = bytearray(0x880); c[:4] = ss.CTL_MAGIC; struct.pack_into('<I', c, 0x20, 0x880); struct.pack_into('<I', c, 0x704, code_len); c[0x800] = tag
    return bytes(c)


def code(n, tag):   # a vertex code area: the header magic, the SPH's stage word at +0x30 (vertex), then filler
    c = bytearray(n); c[:4] = ss.GFX_MAGIC; struct.pack_into('<I', c, 0x30, 1); c[0x40] = tag
    return bytes(c)


def table(progs):   # the shader table: per program (code length, code offset, control length, control offset), four entries of type 0x0db4
    return b''.join(struct.pack('<3I', 1 | 0x0db4 << 8 | 1 << 24, kn, ko) + struct.pack('<3I', 2 | 0x0db4 << 8 | 2 << 24, cn, co)
                    + struct.pack('<3I', 1 | 0x0db4 << 8 | 3 << 24, kn, 0xffffffff) + struct.pack('<3I', 2 | 0x0db4 << 8 | 4 << 24, cn, 0xffffffff) for kn, ko, cn, co in progs)


class Frames(unittest.TestCase):
    def test_padded_frames_are_one_stream(self):
        a, b = os.urandom(5000), b'x' * 70000   # stored blocks, then an RLE-like compressible block
        for z in (zs, zs_check):
            d = z(a) + bytes(5) + z(b) + bytes(3)
            self.assertEqual(ss.zstd_frame_len(d, 0), len(z(a)))
            self.assertEqual(ss.nlg4_unzs(d), a + b)

    def test_other_bytes_are_named(self):
        with self.assertRaisesRegex(SystemExit, r'not a zstd frame at 0x'): ss.nlg4_unzs(zs(b'abc') + b'\x01junk')


class Archive(unittest.TestCase):
    def write(self, d, name, data): Path(d, name).write_bytes(data)

    def test_base_and_patch(self):
        k0, k1, k2 = code(0x100, 1), code(0x80, 2), code(0x200, 3)
        c0, c1, c2 = ctl(0x100, 1), ctl(0x80, 2), ctl(0x200, 3)
        # base sections in one stream: something, the code areas, something, the control sections
        lead, mid = os.urandom(0x40).replace(ss.GFX_MAGIC, b'\0' * 4), b'\0' * 0x30
        data = lead + k0 + k1 + mid + c0 + c1
        idx = b'\0' * 0x24 + table([(0x100, 0, 0x880, 0), (0x80, 0x100, 0x880, 0x880)]) + b'\0' * 12
        head = ss.NLG_DICT + ss.NLG4_DICT + b'\x03\x00' + bytes(0x38)
        with tempfile.TemporaryDirectory() as d:
            self.write(d, 'init.dict', head + zs(idx)); self.write(d, 'init.data', zs(data[:0x90]) + bytes(4) + zs(data[0x90:]))
            out = []; ss.nlg_shaders(os.path.join(d, 'init.dict'), '/init.dict', out)
            self.assertEqual([(st, c, k) for _, st, c, k in out], [('vs', c0, k0), ('vs', c1, k1)])
            # the update: program 0 unchanged, program 1 with a new code area and control section, read past the old lengths (0x180, 0x1100) in its body
            body = struct.pack('<I', 0) * 3 + table([(0x100, 0, 0x880, 0), (0x200, 0x180, 0x880, 0x1100)]) + b'\0' * 12 + k2 + c2
            self.write(d, 'init.patch', ss.NLG_DICT + ss.NLG4_PATCH + b'\x03\x00' + struct.pack('<I', len(body)) + bytes(12) + zs(body))
            out = []; ss.nlg_shaders(os.path.join(d, 'init.dict'), '/init.dict', out)
            self.assertEqual([(st, c, k) for _, st, c, k in out], [('vs', c0, k0), ('vs', c2, k2)])

    def test_no_shader_table_is_quiet(self):
        with tempfile.TemporaryDirectory() as d:
            self.write(d, 'a.dict', ss.NLG_DICT + ss.NLG4_DICT + b'\x03\x00' + bytes(0x38) + zs(bytes(0x100)))
            self.write(d, 'a.data', zs(os.urandom(0x200).replace(b'BNSH', b'\0' * 4)))
            out = []; ss.nlg_shaders(os.path.join(d, 'a.dict'), '/a.dict', out)
            self.assertEqual(out, [])

    def test_bnsh_in_the_data_and_the_patch_body(self):   # NintendoWare Ui2d / Font containers in an archive without a shader table (Strikers' FrontEnd/Persistent)
        bnsh = b'BNSH\0\0\0\0' + bytes(range(256))
        with tempfile.TemporaryDirectory() as d, mock.patch.object(ss, 'scan_blob') as scan:
            self.write(d, 'a.dict', ss.NLG_DICT + ss.NLG4_DICT + b'\x03\x00' + bytes(0x38) + zs(bytes(0x100)))
            self.write(d, 'a.data', zs(b'\1' * 0x40 + bnsh))
            body = b'\2' * 0x20 + bnsh
            self.write(d, 'a.patch', ss.NLG_DICT + ss.NLG4_PATCH + b'\x03\x00' + struct.pack('<I', len(body)) + bytes(12) + zs(body))
            out = []; ss.nlg_shaders(os.path.join(d, 'a.dict'), '/a.dict', out)
            self.assertEqual([(c.args[0], c.args[1]) for c in scan.call_args_list], [(b'\1' * 0x40 + bnsh, '/a.dict'), (body, '/a.dict.patch')])


if __name__ == '__main__':
    unittest.main()
