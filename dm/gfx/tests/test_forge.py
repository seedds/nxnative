"""forge.py and unified shader_split.forge_jobs on synthetic Scimitar (v25/v31) and Anvil (v28) .forge files:
tables, name records, stored and Oodle-decoded blocks/containers, NVN shader extraction, single-stage cover pairing,
coexistence of v28 and v25/v31, unknown-version / truncated error jobs, and damaged files."""
import contextlib
import io
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'dm/gfx/tools'))
sys.path.insert(0, str(ROOT / 'dm/gfx/tests'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '010015100B514000'
import forge, shader_split   # noqa: E402
import test_anvil_forge   # noqa: E402
if _had is None: os.environ.pop('DM_GAME', None)
else: os.environ['DM_GAME'] = _had


def block(chunks, typ=6):   # a v25/v31 compressed block: chunks [(raw size, stored bytes)], each after its u32 checksum
    head = forge.BLOCK + struct.pack('<HBHHH', 1, typ, 0x8000, 0x8000, len(chunks)) + b''.join(struct.pack('<HH', raw, len(s)) for raw, s in chunks)
    return head + b''.join(b'\xcc' * 4 + s for _, s in chunks)


def entry(name, blocks):   # a v25/v31 FILEDATA entry: its 0x1b8-byte header (its name, padding), one 8-byte item, then its blocks
    return b'FILEDATA' + name.encode() + bytes(forge.ENTRY_HEAD - 8 - len(name)) + struct.pack('<II', 1, 0) + b''.join(blocks)


def forge_file(ents, version=31):   # ents: [(name, entry bytes)]; header, file data header and one table, then the entries
    n = len(ents); hdr = 0x20; tab = hdr + 0x28; idx = tab + 0x30; names = idx + 16 * n; data = names + forge.NAME_REC * n
    out = bytearray(forge.MAGIC + struct.pack('<Iq', version, hdr))
    out += bytes(hdr - len(out)) + struct.pack('<IIqqIIq', n, 1, 0, -1, 5000, 1, tab)
    out += struct.pack('<IIqqIIqq', n, 1, idx, -1, 0, 4999, names, data)
    offs = []; p = data
    for _, e in ents: offs.append(p); p += len(e)
    for (nm, e), o in zip(ents, offs): out += struct.pack('<qII', o, 0x1000 + len(offs), len(e) - forge.ENTRY_HEAD)   # the index's size leaves the header out
    for nm, e in ents:
        rec = struct.pack('<IQ', len(e), 0x1234) + bytes(forge.NAME_AT - 12) + nm.encode()
        out += rec + bytes(forge.NAME_REC - len(rec))
    for _, e in ents: out += e
    return bytes(out)


def shader(stage='vs', seed=1, code_len=0xa0):   # a valid NVN control section (0x880 bytes) + headed code area
    sph_stage = {'vs': 1, 'ps': 5}[stage]
    sph = bytearray(0x50)
    struct.pack_into('<I', sph, 0, sph_stage << 10)
    if stage == 'ps':
        struct.pack_into('<I', sph, 4 * 6, 0x0000ff00)   # read generic input attribute 1 so pixel_input_cover selects it
    code = forge.GFX_MAGIC + bytes(0x2c) + bytes(sph) + bytes([seed]) * (code_len - 0x80)
    ctl = bytearray(forge.CTL_MAGIC + bytes(0x87c))
    struct.pack_into('<I', ctl, 0x20, 0x880)
    struct.pack_into('<I', ctl, 0x704, len(code))
    return bytes(ctl), code


class ForgeTests(unittest.TestCase):
    def test_entries(self):
        for ver, typ in ((31, 6), (25, 4)):
            title = entry('TitleScreen', [block([(3, b'abc')], typ=typ)])
            d = forge_file([('GlobalMetaFile', entry('GlobalMetaFile', [])), ('TitleScreen', title)], version=ver)
            self.assertTrue(forge.is_forge(d))
            self.assertEqual(forge.version(d), ver)
            es = forge.entries(d)
            self.assertEqual([n for _, _, n in es], ['GlobalMetaFile', 'TitleScreen'])
            self.assertEqual(d[es[1][0]:es[1][0] + es[1][1]], title)   # the size read includes the header
            self.assertEqual(forge.read(d, *es[1][:2], None, version=ver), [b'abc'])

    def test_blocks_stored_and_decoded(self):   # a chunk whose stored size is its raw size is taken as it is; the others go to the decoder with their raw size
        e = entry('a', [block([(2, b'hi')]), block([(5, b'\x8c\x0aXY'), (3, b'end')], typ=4)])
        seen = []
        def decode(comp, n): seen.append((comp, n)); return b'hello'[:n]
        self.assertEqual(forge.read(e, 0, len(e), decode), [b'hi', b'helloend'])
        self.assertEqual(seen, [(b'\x8c\x0aXY', 5)])
        self.assertEqual(forge.read(entry('tiny', []), 0, forge.ENTRY_HEAD + 8, decode), [])

    def test_nvn_shaders(self):
        ctl, code = shader('vs', 1, 0xa0); ctl2, code2 = shader('ps', 2, 0xc0)
        b = b'material' + ctl + code + b'\0' * 7 + ctl2 + code2 + b'tail'
        found, bad = forge.nvn_shaders(b)
        self.assertEqual([(c, x, y) for c, x, y in found], [(8, ctl, code), (8 + len(ctl) + len(code) + 7, ctl2, code2)])
        self.assertEqual(bad, [])
        self.assertEqual(forge.nvn_shaders(forge.CTL_MAGIC + bytes(0x800)), ([], []))   # the magic in other data: no control section's length
        found, bad = forge.nvn_shaders(ctl + b'no code here' + bytes(0xa0))
        self.assertEqual(found, []); self.assertEqual(len(bad), 1)

    def test_damaged_and_unknown_version(self):
        e = entry('t', [block([(3, b'abc')])])
        d = forge_file([('t', e)])
        with self.assertRaises(ValueError): forge.entries(b'scimitaR\0' + d[9:])   # not a forge
        with self.assertRaises(ValueError): forge.entries(forge_file([('t', e)], version=29))   # unknown forge version
        with self.assertRaises(ValueError): forge.entries(d[:len(d) - len(e) - 1])   # the entry runs past the end
        bad = bytearray(d); struct.pack_into('<I', bad, 0x20, 2)   # the header counts two entries
        with self.assertRaises(ValueError): forge.entries(bytes(bad))
        with self.assertRaises(ValueError): forge.read(entry('t', [block([(3, b'abc')], typ=9)]), 0, 0x200, None)   # an unknown block type
        cut = entry('t', [block([(3, b'abc')])])[:-2]
        with self.assertRaises(ValueError): forge.read(cut, 0, len(cut), None)   # a chunk runs past the entry
        with self.assertRaises(ValueError): forge.read(entry('t', [block([(5, b'abc')])]), 0, 0x200, lambda c, n: b'ab')   # decodes to 2 bytes, not 5

    def test_unified_jobs_v31_v28_and_single_stage_cover(self):
        vs_ctl, vs_code = shader('vs', 1)
        ps_ctl, ps_code = shader('ps', 2)
        scim_blob = b'mat' + vs_ctl + vs_code + ps_ctl + ps_code
        fake_oodle = b'\x8c\x0cSCIM'
        scim_forge = forge_file([('TitleMat', entry('TitleMat', [block([(len(scim_blob), fake_oodle)], typ=6)]))], version=31)

        v28_code, v28_ctl = test_anvil_forge.make_shader_blob('vs', 9)
        v28_rec = test_anvil_forge.make_gpucodeonly(v28_code, v28_ctl)
        v28_forge = test_anvil_forge.make_forge([(0x111, test_anvil_forge.make_container([(len(v28_rec), v28_rec)]))])

        with tempfile.TemporaryDirectory() as d:
            p31 = os.path.join(d, 'DataNX_AC2.forge')
            p28 = os.path.join(d, 'DataPC_Fenyx.forge')
            Path(p31).write_bytes(scim_forge)
            Path(p28).write_bytes(v28_forge)
            jobs, taken = shader_split.forge_jobs([(p31, 'DataNX_AC2.forge'), (p28, 'DataPC_Fenyx.forge')], parts=4)
            self.assertEqual(taken, {p31, p28})
            keys = {}
            with mock.patch.object(shader_split, 'oodle', side_effect=lambda blk, n: scim_blob if blk == fake_oodle and n == len(scim_blob) else b''):
                for job in jobs:
                    _, found, _, _ = shader_split.scan_file_in(job)
                    keys.update(found)
            self.assertEqual(len(keys), 3)
            scim_vs = [k for k, v in keys.items() if '#SCIMITAR:TitleMat@' in v[0] and v[1] == 'vs']
            scim_ps = [k for k, v in keys.items() if '#SCIMITAR:TitleMat@' in v[0] and v[1] == 'ps']
            v28_vs = [k for k, v in keys.items() if '#GPUCODEONLY@' in v[0] and v[1] == 'vs']
            self.assertEqual((len(scim_vs), len(scim_ps), len(v28_vs)), (1, 1, 1))
            pairs, cover = shader_split.separable_pairs(keys)
            self.assertEqual(cover, scim_ps)
            self.assertEqual(pairs, {(scim_vs[0], scim_ps[0])})   # #SCIMITAR: gets cover pairing; v28 #GPUCODEONLY@ does not

    def test_unknown_version_and_truncated_report_unreadable(self):
        e = entry('t', [block([(3, b'abc')])])
        v29 = forge_file([('t', e)], version=29)
        with tempfile.TemporaryDirectory() as d:
            p_bad_ver = os.path.join(d, 'Unknown.forge')
            p_short = os.path.join(d, 'Short.forge')
            Path(p_bad_ver).write_bytes(v29)
            Path(p_short).write_bytes(forge.MAGIC + b'\x01')
            jobs, taken = shader_split.forge_jobs([(p_bad_ver, 'Unknown.forge'), (p_short, 'Short.forge')])
            self.assertEqual(taken, {p_bad_ver, p_short})
            self.assertEqual(len(jobs), 2)
            with contextlib.redirect_stdout(io.StringIO()):
                results = [shader_split.scan_file(j) for j in jobs]
            self.assertEqual([r[3][0] for r in results], ['unreadable', 'unreadable'])
            self.assertIn('forge version 29', results[0][3][1])
            self.assertIn('header runs past the end', results[1][3][1])


if __name__ == '__main__':
    unittest.main()
