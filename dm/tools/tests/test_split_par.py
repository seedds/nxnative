"""shader_split: Ryu Ga Gotoku's PARC archives (Yakuza Kiwami's data/shader/*.par) and SLLZ compression. A PARC archive's files are listed (par_entries), and only those
whose first bytes are a shader object (GSVS4 / GSPS4 / GSFX4), a GLSLC output or another PARC archive are decompressed and read (par_shaders); the engine's own shader objects
are SLLZ streams in main (sllz_main_objects), which raw_shaders leaves out. The SLLZ encoder here mirrors ParManager's ParLibrary (Sllz/Compressor.cs). No game files needed."""
import contextlib
import io
import os
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '0100199023B2E000'   # shader_split loads a profile at import; other tests expect no default game
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had

GLSLC = b'\x91\x68\x86\x19'


def sllz_header(version, n, stored, be):
    e = '>' if be else '<'
    return b'SLLZ' + bytes([1 if be else 0, version]) + struct.pack(e + 'HII', 16, n, stored)


def sllz1(data, be=False):
    """SLLZ version 1 as ParLibrary writes it: the next flag byte is reserved when a flag byte's eighth bit is used, before that item's bytes"""
    body, flag_at, flag, bits, i = bytearray(b'\0'), 0, 0, 0, 0
    while i < len(data):
        best = (0, 0)
        for dist in range(1, min(i, 4096) + 1):
            n = 0
            while n < 18 and i + n < len(data) and data[i + n - dist] == data[i + n]: n += 1
            if n > best[1]: best = (dist, n)
        copy = best[1] >= 3
        if copy: flag |= 0x80 >> bits
        bits += 1
        if bits == 8: body[flag_at] = flag; flag, bits, flag_at = 0, 0, len(body); body.append(0)
        if copy: body += struct.pack('<H', (best[0] - 1) << 4 | (best[1] - 3)); i += best[1]
        else: body.append(data[i]); i += 1
    body[flag_at] = flag
    return sllz_header(1, len(data), 16 + len(body), be) + bytes(body)


def sllz2(data, chunk=0x100, stored_chunk=1, be=False):
    """SLLZ version 2: zlib chunks behind {u24 big-endian chunk size with these 5 bytes (bit 23: stored), u16 big-endian decoded size - 1}; chunk number stored_chunk is stored as it is"""
    body = b''
    for k, o in enumerate(range(0, len(data), chunk)):
        part = data[o:o + chunk]
        if k == stored_chunk: body += (0x800000 | 5 + len(part)).to_bytes(3, 'big') + (len(part) - 1).to_bytes(2, 'big') + part
        else: z = zlib.compress(part); body += (5 + len(z)).to_bytes(3, 'big') + (len(part) - 1).to_bytes(2, 'big') + z
    return sllz_header(2, len(data), 16 + len(body), be) + body


def parc(root_files, sub_files=(), be=True):
    """A PARC archive: root_files in the root folder, sub_files in its folder 'sub'; each file (name, stored bytes, decoded length or None when stored as it is)"""
    e = '>' if be else '<'
    files = list(root_files) + list(sub_files); nfo, nfi = (2 if sub_files else 1), len(files)
    names = b''.join(n.encode().ljust(0x40, b'\0') for n in (['.', 'sub'][:nfo] + [f[0] for f in files]))
    fo = 0x20 + len(names); fi = fo + 0x20 * nfo; data_at = (fi + 0x20 * nfi + 0x7ff) & ~0x7ff
    folders = struct.pack(e + '8I', nfo - 1, 1, len(root_files), 0, 0x10, 0, 0, 0)
    if sub_files: folders += struct.pack(e + '8I', 0, 0, len(sub_files), len(root_files), 0x10, 0, 0, 0)
    table, blob = b'', b''
    for name, stored, n in files:
        off = data_at + len(blob); blob += stored + bytes(-len(stored) & 0xf)
        table += struct.pack(e + '6IQ', 0x80000000 if n is not None else 0, len(stored) if n is None else n, len(stored), off & 0xffffffff, 0x20, off >> 32, 0)
    head = b'PARC' + bytes([2, 1 if be else 0, 0, 1]) + struct.pack(e + '6I', 0x20001, 0, nfo, fo, nfi, fi)
    out = head + names + folders + table
    return out + bytes(data_at - len(out)) + blob


def packed(data, v=1):
    return (sllz1(data) if v == 1 else sllz2(data)), len(data)


VSO = b'GSVS4\0\0\0' + bytes(0x18) + GLSLC + b'vertex' * 30
PSO = b'GSPS4\0\0\0' + bytes(0x18) + GLSLC + b'pixel!' * 30
FXO = b'GSFX4\0\0\0' + bytes(0x18) + GLSLC + b'vertex' * 10 + GLSLC + b'pixel!' * 10
DDS = b'DDS |' + bytes(range(256)) * 8


class Recorder:
    """glslc_shaders replaced: the label and bytes of each call. sllz wrapped: the labels it decoded whole"""
    def __enter__(self):
        self.seen, self.whole = [], []
        self.g, self.s = ss.glslc_shaders, ss.sllz
        ss.glslc_shaders = lambda d, label, need=True, skip=(), ranges=None, offsets=None: self.seen.append((label, bytes(d))) or []
        def sllz(d, label, peek=0, max_out=ss.SLLZ_MAX_OUT):
            if not peek: self.whole.append(label)
            return self.s(d, label, peek, max_out)
        ss.sllz = sllz
        return self

    def __exit__(self, *a): ss.glslc_shaders, ss.sllz = self.g, self.s


class Sllz(unittest.TestCase):
    def test_version_1_round_trips(self):
        for data in (b'', b'x', b'ab' * 50 + b'tail', bytes(range(256)) * 3, b'GSVS4' + b'\0' * 200 + b'abcabcabcabd' * 9):
            for be in (False, True):
                self.assertEqual(ss.sllz(sllz1(data, be), 'x'), data)

    def test_version_1_copies_overlap(self):   # distance 1, length 18: one byte repeated, every copied byte read after it is written
        self.assertEqual(ss.sllz(sllz1(b'z' * 100), 'x'), b'z' * 100)

    def test_version_2_chunks(self):
        data = bytes(range(256)) * 5 + b'end'
        for be in (False, True): self.assertEqual(ss.sllz(sllz2(data, be=be), 'x'), data)

    def test_peek_reads_only_the_first_bytes(self):
        data = VSO * 4
        self.assertEqual(ss.sllz(sllz1(data)[:0x40], 'x', peek=8), data[:8])
        self.assertEqual(ss.sllz(sllz2(data, stored_chunk=-1)[:0x200], 'x', peek=8), data[:8])

    def test_damaged_streams(self):
        whole = sllz1(bytes(range(200)))
        with self.assertRaises(SystemExit): ss.sllz(whole[:len(whole) // 2], 'x')                    # it ends early
        self.assertEqual(ss.sllz(whole[:len(whole) // 2], 'x', peek=8), bytes(range(8)))              # a peek takes what is there
        with self.assertRaises(SystemExit): ss.sllz(whole[:5] + b'\3' + whole[6:], 'x')             # version 3
        with self.assertRaises(SystemExit): ss.sllz(sllz_header(1, 4, 18, False) + b'\x80\x00\x00', 'x')   # a copy from before the start
        with self.assertRaises(SystemExit): ss.sllz(b'NOPE' + bytes(12), 'x')
        self.assertEqual(ss.sllz(sllz_header(1, 0, 16, False), 'x'), b'')                              # an empty file, no flag byte
        bad2 = sllz_header(2, 8, 21, False) + b'\0\0\2' + bytes(2)                                  # a chunk shorter than its own 5 bytes
        self.assertEqual(ss.sllz(bad2, 'x', peek=8), b'')
        with self.assertRaises(SystemExit): ss.sllz(bad2, 'x')
        overread = sllz_header(2, 10, 16 + 5 + 10, False) + (0x800005).to_bytes(3, 'big') + (9).to_bytes(2, 'big') + b'ABCDEFGHIJ'
        with self.assertRaises(SystemExit): ss.sllz(overread, 'x')                                  # stored chunk must equal 5 + u
        with self.assertRaises(SystemExit): ss.sllz(sllz_header(2, 4, 100, False) + b'\0' * 10, 'x')  # declared stored past input len
        bomb = zlib.compress(b'A' * 4096)
        bomb_pkt = sllz_header(2, 16, 16 + 5 + len(bomb), False) + (5 + len(bomb)).to_bytes(3, 'big') + (15).to_bytes(2, 'big') + bomb
        with self.assertRaises(SystemExit): ss.sllz(bomb_pkt, 'x')                                  # zlib bomb exceeding declared u
        with self.assertRaises(SystemExit): ss.sllz(sllz_header(2, ss.SLLZ_MAX_OUT + 1, 20, False) + b'\0' * 4, 'x')


class Parc(unittest.TestCase):
    def test_entries_in_both_byte_orders(self):
        for be in (True, False):
            d = parc([('a.pso', *packed(PSO)), ('b.dds', DDS, None)], [('c.vso', VSO, None)], be=be)
            ents = ss.par_entries(io.BytesIO(d), 0, 'x')
            assert ents is not None
            self.assertEqual([(p, c) for p, c, _, _ in ents], [('/a.pso', True), ('/b.dds', False), ('/sub/c.vso', False)])
            _, _, off, n = ents[2]; self.assertEqual(d[off:off + n], VSO)
        self.assertIsNone(ss.par_entries(io.BytesIO(b'PAK\1' + bytes(60)), 0, 'x'))

    def test_only_shader_objects_are_decompressed(self):
        inner = parc([('b.vso', VSO, None), ('t.dds', DDS, None)])
        inner_packed = parc([('c.fxo', *packed(FXO))])
        d = parc([('a.pso', *packed(PSO)), ('big.dds', *packed(DDS)), ('inner.par', inner, None)], [('packed.par', *packed(inner_packed, 2))])
        with Recorder() as r:
            out = []; self.assertTrue(ss.par_shaders(io.BytesIO(d), 0, '/x.par', out))
        self.assertEqual(r.seen, [('/x.par/a.pso', PSO), ('/x.par/inner.par/b.vso', VSO), ('/x.par/sub/packed.par/c.fxo', FXO)])
        self.assertNotIn('/x.par/big.dds', r.whole)   # only peeked at

    def test_a_damaged_file_is_left_out_alone(self):
        bad = sllz1(PSO); bad = bad[:16] + bad[16:40] + b'\xff' * 8   # its first bytes still decode, the rest does not
        d = parc([('bad.pso', bad, len(PSO)), ('good.vso', *packed(VSO))])
        with Recorder() as r, contextlib.redirect_stdout(io.StringIO()) as log:
            ss.par_shaders(io.BytesIO(d), 0, '/x.par', [])
        self.assertEqual([lab for lab, _ in r.seen], ['/x.par/good.vso'])
        self.assertIn('[RED] /x.par/bad.pso', log.getvalue())

    def test_scan_reads_a_par_file_and_an_sllz_one(self):
        d = parc([('a.pso', *packed(PSO)), ('t.dds', *packed(DDS))])
        with tempfile.TemporaryDirectory() as t:
            for name, body in (('x.par', d), ('wrapped.par', sllz2(d, chunk=0x1000))):
                p = os.path.join(t, name)
                with open(p, 'wb') as f: f.write(body)
                with Recorder() as r: rel = ss.scan_file_in((p, 'data/shader/' + name))[0]
                self.assertEqual(rel, 'data/shader/' + name)
                self.assertEqual(r.seen, [(f'/data/shader/{name}/a.pso', PSO)])

    def test_dag_folder_tree_and_self_referential_parc_bounded(self):
        nfo = 30
        names = b''.join(f'd{i}'.encode().ljust(0x40, b'\0') for i in range(nfo))
        fo = 0x20 + len(names); fi = fo + 0x20 * nfo
        folders = b''.join(struct.pack('>8I', 2 if i + 2 < nfo else 0, i + 1, 0, 0, 0x10, 0, 0, 0) for i in range(nfo))
        dag = b'PARC\2\1\0\1' + struct.pack('>6I', 0x20001, 0, nfo, fo, 0, fi) + names + folders
        t0 = __import__('time').monotonic()
        self.assertEqual(ss.par_entries(io.BytesIO(dag), 0, '/dag.par'), [])
        self.assertLess(__import__('time').monotonic() - t0, 0.5)
        self_ref = parc([('loop.par', b'PARC', None)])
        # Patch entry 0 to point at base offset 0 with size len(self_ref) (uncompressed self-reference)
        fi_off = 0x20 + 0x40 * 2 + 0x20
        bad = bytearray(self_ref)
        struct.pack_into('>6IQ', bad, fi_off, 0, len(self_ref), len(self_ref), 0, 0x20, 0, 0)
        with Recorder(), contextlib.redirect_stdout(io.StringIO()) as log:
            ss.par_shaders(io.BytesIO(bytes(bad)), 0, '/self.par', [])
        self.assertIn('[RED] /self.par/loop.par: self-referential PARC archive', log.getvalue())

    def test_large_non_shader_sllz_peek_does_not_fail(self):
        big = bytearray(sllz1(DDS))
        big_n = ss.SLLZ_MAX_OUT + 100
        struct.pack_into('<I', big, 8, big_n)
        big_dds = bytes(big)
        self.assertEqual(ss.sllz(big_dds, 'big.dds', peek=8), DDS[:8])
        with self.assertRaises(SystemExit): ss.sllz(big_dds, 'big.dds')
        d = parc([('big.dds', big_dds, big_n), ('a.pso', *packed(PSO))])
        with Recorder() as r, contextlib.redirect_stdout(io.StringIO()) as log:
            ss.par_shaders(io.BytesIO(d), 0, '/x.par', [])
        self.assertEqual(r.seen, [('/x.par/a.pso', PSO)])
        self.assertNotIn('[RED]', log.getvalue())

    def test_quota_exceeded_aborts_par_shaders_immediately(self):
        inner = parc([('in1.dds', DDS, None), ('in2.dds', DDS, None)])
        outer = parc([('inner.par', inner, None), ('after.pso', *packed(PSO))])
        for quota, msg in (([2, ss.PAR_MAX_BYTES], 'PARC entry budget exceeded'),
                           ([100, 100], 'PARC byte budget exceeded')):
            with Recorder() as r, contextlib.redirect_stdout(io.StringIO()) as log:
                with self.assertRaises(SystemExit) as cm:
                    ss.par_shaders(io.BytesIO(outer), 0, '/q.par', [], quota=quota)
            self.assertIn(msg, str(cm.exception))
            self.assertEqual(r.seen, [])   # aborted before reaching sibling 'after.pso'
            self.assertEqual(log.getvalue(), '')   # not swallowed into per-member print

    def test_overlapping_folder_table_rejected_in_linear_time(self):
        nfo, nfi = 20, 50
        names = b''.join(f'n{i}'.encode().ljust(0x40, b'\0') for i in range(nfo + nfi))
        fo = 0x20 + len(names); fi = fo + 0x20 * nfo
        folders = b''.join(struct.pack('>8I', nfo - 1, 1, nfi, 0, 0x10, 0, 0, 0) for _ in range(nfo))
        files = b''.join(struct.pack('>6IQ', 0, 0, 0, 0, 0x20, 0, 0) for _ in range(nfi))
        evil = b'PARC\2\1\0\1' + struct.pack('>6I', 0x20001, 0, nfo, fo, nfi, fi) + names + folders + files
        with self.assertRaises(SystemExit) as cm:
            ss.par_entries(io.BytesIO(evil), 0, '/evil.par')
        self.assertIn('PARC folder table overlap exceeds budget', str(cm.exception))


class Main(unittest.TestCase):
    def test_sllz_objects_in_main(self):
        vso, dds = sllz1(VSO), sllz1(DDS)
        img = b'\0' * 0x100 + vso + b'\0' * 7 + dds + b'SLLZ\0\3' + bytes(30) + b'SLLZ' + bytes(4)
        self.assertEqual(ss.sllz_streams(img), [(0x100, 0x100 + len(vso)), (0x107 + len(vso), 0x107 + len(vso) + len(dds))])
        with Recorder() as r: ss.sllz_main_objects(img, 'main')
        self.assertEqual(r.seen, [('main@0x100', VSO)])

    def test_raw_shaders_leaves_out_magics_in_sllz_streams(self):   # an LZ literal run copies a control magic into main, 4-byte aligned here
        lit = b'GSVS4\0\0\0' + ss.CTL_MAGIC + bytes(0x40)
        img = bytearray(sllz1(lit)); at = img.find(ss.CTL_MAGIC)
        img = bytes(-at & 3) + bytes(img)
        self.assertEqual(img.find(ss.CTL_MAGIC) & 3, 0)
        with contextlib.redirect_stdout(io.StringIO()) as log: self.assertEqual(ss.raw_shaders(img, 'main'), [])
        self.assertNotIn('[RED]', log.getvalue())


if __name__ == '__main__':
    unittest.main()
