"""Media.Vision MVGL archives (legacy 32-bit/doboz and wide 64-bit/LZ4) and NXVS / NXFS / NXCS shader containers; all bytes are generated."""
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import lz4.block

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '010015100B514000'
import mvgl
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had


def program(stage, tag, extra_ctl=b''):
    code = bytearray(0x100 + tag * 8)
    code[:4] = ss.CS_MAGIC if stage == 'cs' else ss.GFX_MAGIC
    if stage != 'cs': struct.pack_into('<I', code, 0x30, (ss.STAGES.index(stage) + 1) << 10 | 1)
    if stage == 'ps': code[0x48] = 1   # generic input 0, exercising separable_pairs
    code[0x80] = tag
    ctl = bytearray(0x880); ctl[:4] = ss.CTL_MAGIC
    struct.pack_into('<I', ctl, 0x20, len(ctl)); struct.pack_into('<I', ctl, 0x704, len(code))
    if stage == 'cs': struct.pack_into('<3I', ctl, 0x748, 64, 1, 1)
    return bytes(ctl) + extra_ctl, bytes(code)


def nxsh(magic, ctl, code, *, tail=b'', reserved=0):
    coff = 0x40; koff = coff + len(ctl); total = koff + len(code) + len(tail)
    hdr = bytearray(coff); hdr[:4] = magic
    struct.pack_into('<6I', hdr, 4, total, reserved, coff, len(ctl), koff, len(code))
    return bytes(hdr) + ctl + code + tail


def doboz_match_stream():
    """Doboz version 0 compressed stream using literals + 1-byte matches + 8-byte literal tail (smaller than uncompressed)."""
    raw = b'AAAA' + b'A' * 36 + b'12345678'   # 48 bytes: 4 literals, 12 matches of (off=1, len=3), then 8 tail literals
    ctl = (((1 << 12) - 1) << 4) | (1 << 31)  # bits 0..3 literal, 4..15 match, 16..23 tail literal, bit 31 sentinel
    body = struct.pack('<I', ctl) + b'AAAA' + bytes([0x04] * 12) + b'12345678'
    header = bytes([3 << 3]) + struct.pack('<II', len(raw), 9 + len(body))
    return raw, header + body


def make_mvgl(entries, *, wide=True):
    """Build a synthetic legacy (32-bit) or wide (64-bit) MDB1 archive from [(name, ext, payload, compressed_bytes_or_None)]."""
    nt = nn = len(entries) + 1; nd = len(entries)
    hdr_sz, tree_sz, name_sz, data_sz = (0x20, 16, 0x80, 16) if wide else (0x14, 8, 0x40, 12)
    start = hdr_sz + nt * tree_sz + nn * name_sz + nd * data_sz
    tree = bytearray(nt * tree_sz); names = bytearray(nn * name_sz); data = bytearray(nd * data_sz); body = bytearray()
    if wide: struct.pack_into('<4I', tree, 0, 0xffffffff, 0xffffffff, 0, 1 if nd else 0)
    else: struct.pack_into('<4H', tree, 0, 0xffff, 0xffff, 0, 1 if nd else 0)
    for i, (name, ext, raw, comp) in enumerate(entries):
        k = i + 1
        if wide: struct.pack_into('<4I', tree, k * 16, 0, i, 0, k)
        else: struct.pack_into('<4H', tree, k * 8, 0, i, 0, k)
        eb = ext.encode('latin1').ljust(4, b' ')[:4]
        nb = name.encode('latin1')
        names[k * name_sz:k * name_sz + 4] = eb
        names[k * name_sz + 4:k * name_sz + 4 + len(nb)] = nb
        stored = raw if comp is None else comp
        off = len(body); body += stored
        if wide: struct.pack_into('<QII', data, i * 16, off, len(raw), len(stored))
        else: struct.pack_into('<III', data, i * 12, off, len(raw), len(stored))
    total = start + len(body)
    hdr = struct.pack('<4sIIIQQ', b'MDB1', nt, nn, nd, start, total) if wide else struct.pack('<4sHHIII', b'MDB1', nt, nn, nd, start, total)
    return hdr + bytes(tree) + bytes(names) + bytes(data) + bytes(body)


class MvglArchive(unittest.TestCase):
    def test_legacy_and_wide_auto_detection_stored_and_compressed(self):
        raw_a = b'hello world! ' * 30
        raw_b = b'stored-bytes-0123456789'
        raw_doboz, comp_doboz = doboz_match_stream()
        self.assertLess(len(comp_doboz), len(raw_doboz))
        legacy = make_mvgl([
            ('shaders\\a_vp', 'shad', raw_doboz, comp_doboz),
            ('shaders\\b_fp', 'shad', raw_b, None),
        ], wide=False)
        wide = make_mvgl([
            ('shaders\\a_vp', 'shad', raw_a, lz4.block.compress(raw_a, store_size=False)),
            ('shaders\\b_fp', 'shad', raw_b, None),
        ], wide=True)
        with tempfile.TemporaryDirectory() as root:
            p_old = Path(root, 'old.mvgl'); p_old.write_bytes(legacy)
            p_new = Path(root, 'new.mvgl'); p_new.write_bytes(wide)
            with mvgl.Archive(str(p_old)) as a:
                self.assertFalse(a.wide)
                self.assertEqual(a.read('shaders\\a_vp.shad'), raw_doboz)
                self.assertEqual(a.read('shaders\\b_fp.shad'), raw_b)
            with mvgl.Archive(str(p_new)) as a:
                self.assertTrue(a.wide)
                self.assertEqual(a.read('shaders\\a_vp.shad'), raw_a)
                self.assertEqual(a.read('shaders\\b_fp.shad'), raw_b)

    def test_rejects_non_mvgl_and_corrupt_entries(self):
        with tempfile.TemporaryDirectory() as root:
            for name, payload in (('afs2.mvgl', b'AFS2' + bytes(64)), ('bad_hdr.mvgl', b'MDB1' + bytes(64))):
                p = Path(root, name); p.write_bytes(payload)
                with self.assertRaises(ValueError): mvgl.Archive(str(p))
            bad_lz4 = make_mvgl([('shaders\\bad_vp', 'shad', b'x' * 200, b'\xff\xff\xff')], wide=True)
            p_bad = Path(root, 'bad_lz4.mvgl'); p_bad.write_bytes(bad_lz4)
            with mvgl.Archive(str(p_bad)) as a, self.assertRaises(ValueError):
                a.read('shaders\\bad_vp.shad')
            with self.assertRaises(ValueError):
                mvgl.lz4_block(b'\x10', 10000)   # ratio > 255x rejected before allocation


class NxshContainers(unittest.TestCase):
    def test_standalone_nxvs_nxfs_nxcs_and_separable_pairs(self):
        vs = program('vs', 1); ps = program('ps', 2); cs = program('cs', 3)
        found = []
        for magic, pair, name in ((b'NXVS', vs, 'a_vp.shad'), (b'NXFS', ps, 'a_fp.shad'), (b'NXCS', cs, 'c.shad')):
            ss.scan_blob(nxsh(magic, *pair, tail=bytes(0x20)), '/' + name, found)
        keys = ss.uniq_keys(found)
        self.assertEqual(list(keys), [ss.skey(*vs), ss.skey(*ps), ss.skey(*cs)])
        self.assertTrue(all('#NXSH@' in row[0] for row in keys.values()))
        pairs, cover = ss.separable_pairs(keys)
        self.assertEqual(pairs, {(ss.skey(*vs), ss.skey(*ps))})
        self.assertEqual(cover, [ss.skey(*ps)])

    def test_standalone_validation_rejects_malformed_containers(self):
        ctl, code = program('vs', 1)
        good = bytearray(nxsh(b'NXVS', ctl, code))
        for label, bad in (
            ('truncated', good[:0x18]),
            ('size mismatch', good[:-1]),
            ('control outside', good[:0x10] + struct.pack('<I', len(good)) + good[0x14:]),
            ('code outside', good[:0x18] + struct.pack('<I', len(good)) + good[0x1c:]),
            ('overlap', good[:0x14] + struct.pack('<I', 0x40) + good[0x18:]),
            ('stage mismatch', b'NXFS' + good[4:]),
            ('bad ctl magic', good[:0x40] + bytes(4) + good[0x44:]),
        ):
            with self.subTest(label), self.assertRaisesRegex(SystemExit, r'\[RED\]'):
                ss.nxsh_shader(bytes(bad), 0, '/bad.shad', strict=True)

    def test_embedded_in_main_with_extra_ctl_bytes_and_font_false_positive(self):
        # Trailing fastmath options between control +0x20 and code (as in Time Stranger's main)
        vs = program('vs', 1)
        ps = program('ps', 2, extra_ctl=b'(\x00\x00\x00\x01\x00\x00\x00\x01\x00\x00\x00fastmath' + bytes(16))
        cs = program('cs', 3)
        blob = b'NSO0' + bytes(0x3c) + b'NXVS' + b'\x01\x4a\x78\xee' + bytes(0x38) + nxsh(b'NXVS', *vs) + nxsh(b'NXFS', *ps) + nxsh(b'NXCS', *cs)
        found = []
        ss.scan_blob(blob, 'main', found)
        self.assertEqual([st for _, st, _, _ in found], ['vs', 'ps', 'cs'])
        known = {c[:struct.unpack_from('<I', c, 0x20)[0]] for _, _, c, _ in found}
        self.assertEqual(ss.raw_shaders(blob, 'main', known), [])

    def test_cli_split_wide_mvgl_and_embedded_main(self):
        vs_rom = program('vs', 1); ps_rom = program('ps', 2)
        vs_main = program('vs', 4); ps_main = program('ps', 5, extra_ctl=bytes(0x28)); cs_main = program('cs', 6)
        b_vs = nxsh(b'NXVS', *vs_rom); b_ps = nxsh(b'NXFS', *ps_rom)
        archive = make_mvgl([
            ('shaders\\m_vp', 'shad', b_vs, lz4.block.compress(b_vs, store_size=False)),
            ('shaders\\m_fp', 'shad', b_ps, None),
            ('font\\commonfont', 'bin', b'NXVS' + bytes(0x100), None),
        ], wide=True)
        # Minimal valid NSO0 whose read-only segment holds embedded NXVS / NXFS / NXCS
        ro = nxsh(b'NXVS', *vs_main) + nxsh(b'NXFS', *ps_main) + nxsh(b'NXCS', *cs_main)
        nso_hdr = bytearray(0x100); nso_hdr[:4] = b'NSO0'
        struct.pack_into('<3I', nso_hdr, 0x20, 0x100, 0x1000, len(ro))            # ro: file_off, mem_off, size
        data_mem = (0x1000 + len(ro) + 0xfff) & ~0xfff
        struct.pack_into('<3I', nso_hdr, 0x30, 0x100 + len(ro), data_mem, 0)      # data: page-aligned after ro
        struct.pack_into('<I', nso_hdr, 0x64, len(ro))                            # ro file size (uncompressed)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); rom = root / 'RomFS'; rom.mkdir()
            (rom / 'pack.nx64.mvgl').write_bytes(archive)
            (rom / 'sound.nx64.mvgl').write_bytes(b'AFS2' + bytes(64))   # non-MDB1 .mvgl ignored cleanly
            main = root / 'main'; main.write_bytes(bytes(nso_hdr) + ro)
            out = [root / n for n in ('pkg', 'map.tsv', 'facts.tsv', 'pairs.tsv', 'pso.tsv')]
            r = subprocess.run([sys.executable, REPO / 'dm/gfx/tools/shader_split.py', rom, main, *out],
                               env=dict(os.environ, DM_GAME='010015100B514000', J='2'),
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertNotIn('[RED]', r.stdout)
            rows = [line.split('\t') for line in out[1].read_text().splitlines() if not line.startswith('#')]
            got = {row[3]: row[4] for row in rows if row[0].endswith('.bin')}
            self.assertEqual(set(got), {ss.skey(*vs_rom), ss.skey(*ps_rom), ss.skey(*vs_main), ss.skey(*ps_main), ss.skey(*cs_main)})
            self.assertIn('/pack.nx64.mvgl/shaders\\m_vp.shad', got[ss.skey(*vs_rom)])
            self.assertIn('/pack.nx64.mvgl/shaders\\m_fp.shad', got[ss.skey(*ps_rom)])
            self.assertTrue(got[ss.skey(*cs_main)].startswith('main#NXSH@'))


if __name__ == '__main__':
    unittest.main()
