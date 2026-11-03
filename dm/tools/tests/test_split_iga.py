"""shader_split / iga.py: Alchemy IGA version 13 archives (Crash Team Racing Nitro-Fueled's .pak files): the u8 / u16 block tables chosen by a file's size, stored and LZMA blocks,
and the NVN control sections each followed by its code area in an IGZ file. Synthetic archives only; no game files needed."""
import lzma
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '0100199023B2E000'   # shader_split loads a profile at import; other tests expect no default game
import iga
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had

SECT = 0x800


def lzma_block(data):
    props = {'id': lzma.FILTER_LZMA1, 'lc': 3, 'lp': 0, 'pb': 2, 'dict_size': 0x8000}
    raw = lzma.compress(data, format=lzma.FORMAT_RAW, filters=[props])
    return struct.pack('<I', len(raw)) + bytes([(2 * 5 + 0) * 9 + 3]) + struct.pack('<I', 0x8000) + raw


def archive(files):
    """An IGA v13 archive of files [(name, data, compressed)]: each file at a sector boundary, its blocks at sector boundaries inside it, one u8 table (every file here is under 0x7f sectors)"""
    n = len(files); small, body, ents, at = bytearray(), bytearray(), [], 0
    for name, data, packed in files:
        first = len(small); start = at; cur = bytearray()
        for k in range(0, len(data), iga.BLOCK):
            blk = lzma_block(data[k:k + iga.BLOCK]) if packed else data[k:k + iga.BLOCK]
            small.append(len(cur) // SECT | (0x80 if packed else 0)); cur += blk + bytes(-len(blk) % SECT)
        body += cur; at += len(cur)
        ents.append((start, len(data), 0x20000000 | first if packed else 0xffffffff))
    head_len = iga.HEAD.size + 20 * n + len(small); data_off = (head_len + SECT - 1) // SECT * SECT
    names = b''.join(nm.encode() + b'\0' for nm, _, _ in files); offs = []
    p = 4 * n
    for nm, _, _ in files: offs.append(p); p += len(nm) + 1
    name_off = data_off + len(body)
    head = iga.HEAD.pack(iga.MAGIC, 13, head_len - iga.HEAD.size, n, SECT, 0, 0, 0, 0, len(small), name_off, 0, 4 * n + len(names), 1)
    toc = head + b''.join(struct.pack('<I', i) for i in range(n)) + b''.join(struct.pack('<4I', data_off + o, 0, sz, mode) for o, sz, mode in ents) + bytes(small)
    return toc + bytes(data_off - len(toc)) + bytes(body) + struct.pack(f'<{n}I', *offs) + names


def shader(st_code_len=0x100):
    ctl = bytearray(0x850); ctl[:4] = ss.CTL_MAGIC; struct.pack_into('<I', ctl, 0x20, 0x850); struct.pack_into('<I', ctl, 0x704, st_code_len)
    code = bytearray(st_code_len); code[:4] = ss.GFX_MAGIC; struct.pack_into('<I', code, 0x30, 0x461)   # SPH word 0: a vertex shader (type 1)
    return bytes(ctl), bytes(code)


class Iga(unittest.TestCase):
    def setUp(self):
        self.t = tempfile.TemporaryDirectory(); self.dir = Path(self.t.name)

    def tearDown(self): self.t.cleanup()

    def write(self, name, files):
        p = self.dir / name; p.write_bytes(archive(files)); return str(p)

    def test_stored_and_lzma_files_read_back(self):
        big = bytes(range(256)) * 300   # 76,800 bytes: three LZMA blocks
        p = self.write('a.pak', [('x/stored.bin', b'stored bytes' * 10, False), ('x/packed.igz', big, True)])
        es = iga.files(p)
        self.assertEqual([e[0] for e in es], ['x/stored.bin', 'x/packed.igz'])
        self.assertEqual(iga.read(p, es[0]), b'stored bytes' * 10)
        self.assertEqual(iga.read(p, es[1]), big)
        self.assertEqual(len(es[1][4][0]), 3)

    def test_other_layouts_are_refused(self):
        p = self.write('a.pak', [('f', b'data', False)])
        d = bytearray(Path(p).read_bytes()); d[4] = 11; Path(p).write_bytes(d)
        with self.assertRaisesRegex(ValueError, 'version 11'): iga.files(p)
        d[4] = 13; struct.pack_into('<I', d, iga.HEAD.size + 4 + 12, 0x10000000); Path(p).write_bytes(d)   # compression 1
        with self.assertRaisesRegex(ValueError, 'compression 1'): iga.files(p)
        (self.dir / 'b.pak').write_bytes(b'NOPE' + bytes(60))
        with self.assertRaisesRegex(ValueError, 'not an IGA'): iga.files(str(self.dir / 'b.pak'))

    def test_a_damaged_block_is_a_value_error(self):
        p = self.write('a.pak', [('f.igz', bytes(range(256)) * 64, True)])
        e = iga.files(p)[0]; d = bytearray(Path(p).read_bytes()); at = e[1] + e[4][0][0]
        d[at + 4] = 0xff; Path(p).write_bytes(d)   # LZMA properties past lc / lp / pb's range
        with self.assertRaises(ValueError): iga.read(p, e)

    def test_igz_shaders_are_found_in_the_split(self):
        ctl, code = shader()
        igz = b'\x01ZGI' + bytes(0x7c) + ctl + code + bytes(0x40)
        p = self.write('shaders_nx.pak', [('shaders/Upsample.igz', igz, True), ('textures/t.igz', b'\x01ZGI' + bytes(0x1000), True)])
        jobs, archives = ss.iga_jobs([(p, 'archives/shaders_nx.pak')])
        self.assertEqual(archives, {p})
        found = {}
        for job in jobs: found.update(ss.scan_file_in(job)[1])
        self.assertEqual(list(found), [ss.skey(ctl, code)])
        where, st, c, k = found[ss.skey(ctl, code)]
        self.assertEqual((st, c, k), ('vs', ctl, code))
        self.assertIn('#IGZ@0x80', where)

    def test_a_pak_that_is_not_an_iga_archive_is_left_to_the_loose_scan(self):
        (self.dir / 'movie.pak').write_bytes(b'KB2j' + bytes(60))
        self.assertEqual(ss.iga_jobs([(str(self.dir / 'movie.pak'), 'archives/movie.pak')]), ([], set()))


if __name__ == '__main__':
    unittest.main()
