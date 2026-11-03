"""shader_split: Yacht Club Games data archives ('YCD\\0', Mina the Hollower's .pak.yc) are listed by their file table (ycd_files), and the shaders of shaders.pak.yc,
one stage per <name>.vert.bin / .frag.bin, are read as single-stage programs (ycd_shaders); an archive the table does not describe is named when it holds control sections. No game files needed."""
import os
import struct
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '010029B0118E8000'   # shader_split loads a profile at import; other tests expect no default game
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had


def stage(st, ncode=0x100, tag=0):
    """{u32 control length, u32 code length, control section, code area}: an 0x880-byte control section naming its code length (+0x704), and a graphics code area whose SPH word 0 names the stage"""
    ctl = bytearray(0x880); ctl[:4] = ss.CTL_MAGIC; struct.pack_into('<I', ctl, 0x20, 0x880); struct.pack_into('<I', ctl, 0x704, ncode); ctl[0x800] = tag
    code = bytearray(ncode); code[:4] = ss.GFX_MAGIC; struct.pack_into('<I', code, 0x30, {'vs': 0x0461, 'ps': 0x1462}[st])
    return struct.pack('<II', len(ctl), len(code)) + bytes(ctl) + bytes(code)


def archive(files, hsize=0xc0):
    """files: [(name, data)] → a YCD archive as Mina the Hollower's: header, root record, a name and a data record per file (offsets from each record's +8), the names, then the data"""
    table = hsize + 0x10; names_at = table + 0x20 * len(files)
    names = b''.join(n.encode() + b'\0' for n, _ in files)
    data_at = (names_at + len(names) + 0x3f) & ~0x3f
    head = bytearray(hsize); head[:4] = ss.YCD; struct.pack_into('<II', head, 4, 8, hsize)
    root = struct.pack('<IIIi', 0x20 * len(files), len(files), 0x10, table - (hsize + 8))
    recs, blob, np = b'', b'', names_at
    for i, (n, d) in enumerate(files):
        e = table + 0x20 * i
        while (data_at + len(blob)) & 0x3f: blob += b'\0'
        recs += struct.pack('<IIIi', 0, len(n), 0x10, np - (e + 8)) + struct.pack('<IIIi', len(d), 0x40, 0x10, data_at + len(blob) - (e + 0x18))
        np += len(n) + 1; blob += d
    out = bytes(head) + root + recs + names
    return out + bytes(data_at - len(out)) + blob


class Ycd(unittest.TestCase):
    def test_files_are_listed_from_the_table(self):
        files = [('shaders/copy.frag.bin', stage('ps')), ('shaders/copy.frag.meta', b'meta'), ('shaders/shader.db', b'db' * 9)]
        d = archive(files)
        got = ss.ycd_files(d)
        assert got is not None
        self.assertEqual([(n, s) for n, _, s in got], [(n, len(b)) for n, b in files])
        self.assertEqual([d[p:p + s] for _, p, s in got], [b for _, b in files])
        bank = ss.ycd_files(archive(files, hsize=0x140))   # a sound bank's longer header
        assert bank is not None
        self.assertEqual([n for n, _, _ in bank], [n for n, _ in files])

    def test_shaders_are_single_stage_programs(self):
        d = archive([('shaders/copy.frag.bin', stage('ps')), ('shaders/copy.frag.meta', b'meta'), ('shaders/copy.vert.bin', stage('vs', 0x200, 1))])
        got = ss.ycd_shaders(d, '/shaders.pak.yc')
        self.assertEqual([(w.split('#')[0], st) for w, st, _, _ in got], [('/shaders.pak.yc/shaders/copy.frag.bin', 'ps'), ('/shaders.pak.yc/shaders/copy.vert.bin', 'vs')])
        self.assertTrue(all(any(s in w for s in ss.SINGLE_STAGE) for w, _, _, _ in got))
        self.assertEqual([len(code) for _, _, _, code in got], [0x100, 0x200])
        out = []; ss.scan_blob(d, '/shaders.pak.yc', out)
        self.assertEqual(len(out), 2)

    def test_archive_without_shaders_gives_nothing(self):
        self.assertEqual(ss.ycd_shaders(archive([('levels/hub.tlb.yc', b'level')]), '/level_hub.pak.yc'), [])
        self.assertIsNone(ss.ycd_files(b'YCD\0' + bytes(8)))
        self.assertIsNone(ss.ycd_files(b'PAK\0' + bytes(0x100)))

    def test_stage_that_disagrees_with_its_name_is_named(self):
        with self.assertRaisesRegex(SystemExit, 'copy.vert.bin: its code area is a ps shader'):
            ss.ycd_shaders(archive([('shaders/copy.vert.bin', stage('ps'))]), '/shaders.pak.yc')

    def test_record_whose_lengths_do_not_fill_it_is_named(self):
        bad = bytearray(stage('ps')); struct.pack_into('<I', bad, 4, 0x80)
        with self.assertRaisesRegex(SystemExit, 'do not fill'):
            ss.ycd_shaders(archive([('shaders/a.frag.bin', bytes(bad))]), '/shaders.pak.yc')

    def test_unreadable_table_with_control_sections_is_named(self):
        d = bytearray(archive([('shaders/a.frag.bin', stage('ps'))])); struct.pack_into('<I', d, 0xd0 + 0x14, 0x41)   # the data record's kind
        with self.assertRaisesRegex(SystemExit, 'file table'):
            ss.ycd_shaders(bytes(d), '/shaders.pak.yc')


if __name__ == '__main__':
    unittest.main()
