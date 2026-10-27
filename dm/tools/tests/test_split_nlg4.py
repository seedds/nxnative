"""shader_split: Next Level Games archives of index revision 4 (Mario Strikers: Battle League) and their revision 5 patches, read by nlg4_shaders: the shader pairs of
types 1 and 2, the BNSH files of type 65, an update's entry table and data, and the dispatch on byte 5 that keeps Luigi's Mansion 3's revision 1 with nlg_dict. Synthetic
archives, no game files needed."""
import os
import struct
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

ZSTD = ss.zstd_mod()


def code(stage_type, fill):   # a graphics code area: the header magic, then the SPH with its shader type in bits 10..13 of word 0 (1 vertex, 5 pixel)
    return ss.GFX_MAGIC + bytes(0x2c) + struct.pack('<I', stage_type << 10 | 1) + bytes([fill]) * 0x4c


def control(code_len):   # a control section of 0x880 bytes with the code area's length at +0x704
    c = bytearray(0x880); c[:4] = ss.CTL_MAGIC; struct.pack_into('<I', c, 0x20, 0x880); struct.pack_into('<I', c, 0x704, code_len)
    return bytes(c)


def bnsh(vs, ps):   # one BNSH variation with a vertex and a pixel stage: the variation table at +0x100, its program at +0x140, stage descriptors at +0x180 and +0x1a0
    d = bytearray(0x200)
    d[:8] = b'BNSH\0\0\0\0'; d[0x60:0x64] = b'grsc'; struct.pack_into('<IQ', d, 0x7c, 1, 0x100)
    struct.pack_into('<Q', d, 0x110, 0x140)
    struct.pack_into('<Q', d, 0x148, 0x180); struct.pack_into('<Q', d, 0x148 + 8 * 4, 0x1a0)   # stage slots: vs is 0, ps is 4
    for sc, (c, k) in ((0x180, vs), (0x1a0, ps)):
        pctl = len(d); d += c; pcode = len(d); d += k
        struct.pack_into('<QQII', d, sc + 8, pctl, pcode, len(k), len(c))
    struct.pack_into('<I', d, 0x1c, len(d))
    return bytes(d)


def archive(stem, sections, entries):
    """<stem>.dict (index revision 4) and <stem>.data: sections[i] is (data, compressed); entries are (type, section, data offset, length). Compressed sections are one zstd frame each"""
    data, chunks, frames = b'', [], []
    for i, (blob, comp) in enumerate(sections):
        stored = ZSTD.compress(blob) if comp else blob
        chunks.append(struct.pack('<6I4B', len(data), len(stored), len(blob), len(frames), 1 if comp else 0, 0, 0, 1, 0, 0))
        if comp: frames.append(struct.pack('<2I', len(stored), len(blob)))
        data += stored
    ents = b''.join(struct.pack('<3I', t | s << 24, n, o) for t, s, o, n in entries)
    ix = ZSTD.compress(b''.join(chunks) + ents + b''.join(frames) + b'data\0nxpc\0debug\0')
    d = ss.NLG_DICT + bytes([4, 4, 3, 0]) + bytes(32) + struct.pack('<5I', len(ix), 0, len(entries), len(frames), 0) + bytes([len(sections), 4, 16, 0]) + ix
    Path(stem + '.dict').write_bytes(d); Path(stem + '.data').write_bytes(data)
    return stem + '.dict'


def patch(stem, entries, body, nsec):   # <stem>.patch, revision 5: the new entry table, no hash pairs, 20 bytes per section, then the new data
    z = b''.join(struct.pack('<3I', t | s << 24, n, o) for t, s, o, n in entries) + bytes(20 * nsec) + body
    Path(stem + '.patch').write_bytes(ss.NLG_DICT + bytes([4, 5, 3, 0]) + struct.pack('<4I', len(z), 0, len(entries), nsec) + ZSTD.compress(z))


class Nlg4(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(); self.addCleanup(lambda: __import__('shutil').rmtree(self.tmp))
        self.vs, self.ps = code(1, 0xaa), code(5, 0xbb)
        self.ui_vs, self.ui_ps = code(1, 0xcc), code(5, 0xdd)
        self.b = bnsh((control(len(self.ui_vs)), self.ui_vs), (control(len(self.ui_ps)), self.ui_ps))
        record = bytes(16)   # an entry of type 65 that is not a BNSH file
        self.sections = [(b'', False), (self.vs + self.ps, True), (control(len(self.vs)) + control(len(self.ps)), False), (self.b + record, True)]
        self.entries = [(1, 1, 0, len(self.vs)), (2, 2, 0, 0x880), (1, 1, len(self.vs), len(self.ps)), (2, 2, 0x880, 0x880),
                        (65, 3, 0, len(self.b)), (65, 3, len(self.b), len(record))]

    def read(self, path):
        out = []; ss.nlg_shaders(path, '/a.dict', out)
        return {(where, st): ss.skey(ctl, cd) for where, st, ctl, cd in out}

    def test_pairs_and_bnsh(self):
        got = self.read(archive(os.path.join(self.tmp, 'a'), self.sections, self.entries))
        self.assertEqual(got, {
            ('/a.dict#NLG4:2@0x0', 'vs'): ss.skey(control(len(self.vs)), self.vs),
            ('/a.dict#NLG4:2@0x880', 'ps'): ss.skey(control(len(self.ps)), self.ps),
            ('/a.dict#NLG4:3@0x0#BNSH@0x0#var0', 'vs'): ss.skey(control(len(self.ui_vs)), self.ui_vs),
            ('/a.dict#NLG4:3@0x0#BNSH@0x0#var0', 'ps'): ss.skey(control(len(self.ui_ps)), self.ui_ps)})

    def test_patch_reads_new_data_past_the_old_section(self):   # the update's table points the pixel shader's code past section 1's old length: into the patch's data
        p = os.path.join(self.tmp, 'a'); archive(p, self.sections, self.entries)
        new_ps = code(5, 0xee); old1 = len(self.vs) + len(self.ps)
        ents = list(self.entries); ents[2] = (1, 1, old1, len(new_ps))
        patch(p, ents, new_ps, len(self.sections))
        got = self.read(p + '.dict')
        self.assertEqual(got[('/a.dict#NLG4:2@0x880', 'ps')], ss.skey(control(len(new_ps)), new_ps))
        self.assertEqual(got[('/a.dict#NLG4:2@0x0', 'vs')], ss.skey(control(len(self.vs)), self.vs))

    def test_short_index_is_named(self):
        p = archive(os.path.join(self.tmp, 'a'), self.sections, self.entries)
        d = bytearray(Path(p).read_bytes()); struct.pack_into('<I', d, 0x30, 1000); Path(p).write_bytes(d)   # more entries than the index holds
        with self.assertRaisesRegex(SystemExit, r'^\[RED\] /a\.dict: the version 4 index inflates to'): self.read(p)

    def test_revision_byte(self):   # byte 4 is 4 in both revisions: Luigi's Mansion 3's index (byte 5 = 1) stays with nlg_dict
        p = archive(os.path.join(self.tmp, 'a'), self.sections, self.entries)
        with mock.patch.object(ss, 'nlg4_shaders') as v4: ss.nlg_shaders(p, '/a.dict', [])
        v4.assert_called_once()
        d = bytearray(Path(p).read_bytes()); d[5] = 1; Path(p).write_bytes(d)
        with mock.patch.object(ss, 'nlg4_shaders') as v4, mock.patch.object(ss, 'nlg_dict', side_effect=SystemExit('[RED] revision 1')), self.assertRaises(SystemExit):
            ss.nlg_shaders(p, '/a.dict', [])
        v4.assert_not_called()


if __name__ == '__main__': unittest.main()
