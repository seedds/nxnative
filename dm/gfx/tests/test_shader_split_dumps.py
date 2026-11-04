"""shader_split.py's dump source (`DM_NVN_SHDUMP`): a program a run dumped becomes one shader, keyed from its own bytes, and a pair whose bytes do not hash to its file name is refused
instead of becoming a map row that matches no draw."""
import hashlib
import os
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split as split

CTL_LEN = 0x880


def pair(stage='ps', code_len=0x80):
    """A control section and a code area of the shape the host dumps: control +0x20 says 0x880, +0x704 names the code area's length, the code area starts with a stage's magic."""
    ctl = bytearray(CTL_LEN)
    struct.pack_into('<I', ctl, 0x0, 0x98761234)     # CTL_MAGIC
    struct.pack_into('<I', ctl, 0x20, CTL_LEN)       # the section's own length
    struct.pack_into('<I', ctl, 0x704, code_len)     # the code area's length
    code = bytearray(code_len)
    if stage == 'cs':
        struct.pack_into('<I', code, 0x0, 0x12345679)   # CS_MAGIC: a compute code area, no SPH
    else:
        struct.pack_into('<I', code, 0x0, 0x12345678)   # GFX_MAGIC
        struct.pack_into('<I', code, 0x30, {'vs': 1, 'hs': 2, 'ds': 3, 'gs': 4, 'ps': 5}[stage] << 10)   # SPH word 0: stage in bits 10..13
    ctl[0x100] = 0x5a   # a byte only this shader has, so two pairs differ
    return bytes(ctl), bytes(code)


def write(d, name, ctl, code):
    Path(d, name + '.ctl').write_bytes(ctl)
    Path(d, name + '.code').write_bytes(code)


class DumpedShadersTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix='shdump')
        self.addCleanup(shutil.rmtree, self.dir, True)

    def shdump(self):
        return os.path.join(self.dir, 'shdump')

    def test_a_dump_is_one_shader_named_by_its_bytes(self):
        os.mkdir(self.shdump())
        ctl, code = pair('ps')
        write(self.shdump(), split.skey(ctl, code), ctl, code)
        found = split.dumped_shaders(self.shdump())
        self.assertEqual(len(found), 1)
        where, stage, fctl, fcode = found[0]
        self.assertEqual((stage, fctl, fcode), ('ps', ctl, code))
        self.assertEqual(where, f'/shdump/{split.skey(ctl, code)}#RAW@0x0')   # a fixed label: the same dump in another folder makes the same row
        self.assertIn(split.skey(ctl, code), where)

    def test_a_compute_dump_needs_no_stage_header(self):
        os.mkdir(self.shdump())
        ctl, code = pair('cs')
        write(self.shdump(), split.skey(ctl, code), ctl, code)
        found = split.dumped_shaders(self.shdump())
        self.assertEqual([f[1] for f in found], ['cs'])

    def test_the_prefix_the_older_dump_site_wrote_is_accepted(self):
        os.mkdir(self.shdump())
        ctl, code = pair('vs')
        write(self.shdump(), 'SetShaders-' + split.skey(ctl, code), ctl, code)
        self.assertEqual([f[1] for f in split.dumped_shaders(self.shdump())], ['vs'])

    def test_a_pair_whose_bytes_do_not_hash_to_its_name_is_refused(self):
        os.mkdir(self.shdump())
        ctl, code = pair('ps')
        write(self.shdump(), 'deadbeefdeadbeef', ctl, code)
        with self.assertRaises(SystemExit) as e:
            split.dumped_shaders(self.shdump())
        self.assertIn('hashes to ' + split.skey(ctl, code), str(e.exception))

    def test_an_interrupted_dump_is_refused(self):
        os.mkdir(self.shdump())
        ctl, _ = pair('ps')
        Path(self.shdump(), 'deadbeefdeadbeef.ctl').write_bytes(ctl)
        with self.assertRaises(SystemExit) as e:
            split.dumped_shaders(self.shdump())
        self.assertIn('has no .code beside it', str(e.exception))

    def test_an_empty_directory_is_no_shader(self):
        os.mkdir(self.shdump())
        self.assertEqual(split.dumped_shaders(self.shdump()), [])

    def test_the_row_key_is_the_hosts_hash_of_the_bytes(self):
        """The name the row records is the host's key: sha256(control[:nctl] + code)[:16], recomputed here from the bytes rather than trusted from the file name."""
        os.mkdir(self.shdump())
        ctl, code = pair('ps')
        write(self.shdump(), split.skey(ctl, code), ctl, code)
        where = split.dumped_shaders(self.shdump())[0][0]
        self.assertEqual(where.rsplit('/', 1)[1].split('#')[0], hashlib.sha256(ctl[:0x880] + code).hexdigest()[:16])


if __name__ == '__main__':
    unittest.main()
