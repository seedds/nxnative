"""The shader split of a legacy Unreal pak whose index and entries are encrypted (Little Nightmares II: Helios-Switch.pak): the key comes from main and goes with the jobs; a synthetic pak
and main, no game files or keys."""
import contextlib
import io
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import uepak
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '010029B0118E8000'   # shader_split loads a profile at import; other tests expect no default
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had


def aes_encrypt(key, data):
    import ctypes
    data += bytes(-len(data) % 16)
    cc = ctypes.CDLL('/usr/lib/libSystem.B.dylib').CCCrypt
    out = ctypes.create_string_buffer(len(data)); moved = ctypes.c_size_t()
    assert cc(0, 0, 2, key, len(key), None, data, len(data), out, len(data), ctypes.byref(moved)) == 0
    return out.raw[:moved.value]


def fstr(s):
    b = s.encode() + b'\0'
    return struct.pack('<i', len(b)) + b


def legacy_pak(path, key, files):
    """A version 4 pak: each file stored without compression behind a 53-byte entry header, its bytes and the index AES-encrypted with key (the 0x2d footer of Unreal Engine 4.18)"""
    data, index = bytearray(), fstr('../../../') + struct.pack('<i', len(files))
    for name, body in files.items():
        off = len(data)
        data += bytes(53) + aes_encrypt(key, body)
        index += fstr(name) + struct.pack('<qqqi', off, len(body), len(body), 0) + bytes(20) + struct.pack('<BI', 1, 0)
    ioff = len(data)
    data += aes_encrypt(key, bytes(index))
    data += struct.pack('<BIIQQ', 1, uepak.PAK_MAGIC, 4, ioff, len(index)) + bytes(20)
    path.write_bytes(bytes(data))


@unittest.skipUnless(sys.platform == 'darwin', 'CommonCrypto')
class EncryptedLegacyPak(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name); self.key = bytes(range(7, 39))
        self.pak = self.tmp / 'Helios-Switch.pak'
        legacy_pak(self.pak, self.key, {'Engine/GlobalShaderCache-GLSL_SWITCH.bin': b'global shaders', 'Helios/M_A.uexp': b'a material'})
        ro = bytearray(os.urandom(0x2000)); ro[0x400:0x420] = self.key   # the key stored whole in main's read-only data
        self.main = (bytes(0x1000) + bytes(ro), 0x1000, 0x3000)
        ss._PAKS.pop(str(self.pak), None); self.addCleanup(ss._PAKS.pop, str(self.pak), None)

    def jobs(self, mains):
        with patch.object(uepak, 'nso_image', return_value=self.main):
            return ss.ue_legacy_jobs([(str(self.pak), 'Helios-Switch.pak')], mains=mains)

    def test_the_jobs_carry_the_key_from_main(self):
        jobs, taken = self.jobs(['main'])
        self.assertEqual(taken, {str(self.pak)})
        self.assertEqual(sorted(n for _, _, (tag, names, key) in jobs for n in names), ['Engine/GlobalShaderCache-GLSL_SWITCH.bin', 'Helios/M_A.uexp'])
        self.assertEqual({key for _, _, (_, _, key) in jobs}, {self.key})

    def test_the_next_main_is_tried_when_one_holds_no_key(self):
        other = (bytes(0x3000), 0x1000, 0x3000)
        with patch.object(uepak, 'nso_image', side_effect=[other, self.main]):
            jobs, _ = ss.ue_legacy_jobs([(str(self.pak), 'Helios-Switch.pak')], mains=['base main', 'update main'])
        self.assertEqual({key for _, _, (_, _, key) in jobs}, {self.key})

    def test_no_key_in_main_leaves_the_pak_out_with_a_red_line(self):
        said = io.StringIO()
        with contextlib.redirect_stdout(said): self.assertEqual(self.jobs([]), ([], {str(self.pak)}))
        self.assertIn(f'/Helios-Switch.pak: the pak cannot be read ({self.pak}: the index is encrypted and no key in main', said.getvalue())

    def test_a_job_reads_the_encrypted_files_with_its_key(self):
        jobs, _ = self.jobs(['main'])
        seen = []
        with patch.object(ss, 'ue_inline_shaders', side_effect=lambda d, label, out: seen.append((label, d))):
            for path, rel, args in jobs: ss.scan_file((path, rel, args))
        self.assertEqual(sorted(seen), [('/Helios-Switch.pak#Engine/GlobalShaderCache-GLSL_SWITCH.bin', b'global shaders'), ('/Helios-Switch.pak#Helios/M_A.uexp', b'a material')])


if __name__ == '__main__': unittest.main()
