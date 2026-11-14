"""ExeFS patches (exefs_patches.py): the IPS, IPS32 and .pchtxt parsers, matching and conflict rules, and patch_nso.py on a synthetic NSO with patches written into it.
No game files: every module and patch file is built here."""
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import exefs_patches as ep

BID = 'a1b2c3d4e5f60718293a4b5c6d7e8f91'   # the first 16 bytes of the module's build ID; the header's other 16 are zeros
NOP, MOV_X1_X18, SVC_1, RET, LDXR_W1_X2 = 0xD503201F, 0xAA1203E1, 0xD4000021, 0xD65F03C0, 0x885F7C41
FN, RO, DATA, TEXT_DATA = 0x200, 0x2000, 0x3000, 0x1000   # the one function (nop, nop, nop, ret), the ro and data segments, and a data block in the text (0x1000 words that do not decode)
H = ep.NSO_HEADER


def word(w): return struct.pack('<I', w)


def ips(recs, wide=False):
    out = b'IPS32' if wide else b'PATCH'
    for off, data in recs:
        out += off.to_bytes(4 if wide else 3, 'big')
        out += b'\0\0' + data[0].to_bytes(2, 'big') + bytes([data[1]]) if isinstance(data, tuple) else len(data).to_bytes(2, 'big') + data   # a tuple is RLE: (count, byte)
    return out + (b'EEOF' if wide else b'EOF')


def synthetic_nso():
    """An uncompressed NSO: text 0x2000 (MOD0 at 0x100, the function at FN with its FDE, a data block from TEXT_DATA), ro 0x1000 (.eh_frame_hdr, .eh_frame), data 0x1000"""
    text, ro, data = bytearray(0x2000), bytearray(0x1000), bytearray(0x1000)
    text[0:8] = word(0x14000040) + word(0x100)   # b +0x100, then the MOD0 offset
    for i, w in enumerate((NOP, NOP, NOP, RET)): text[FN + 4 * i:FN + 4 * i + 4] = word(w)
    text[TEXT_DATA:] = b'\xff' * (0x2000 - TEXT_DATA)
    hdr, eh = RO, RO + 0x40
    text[0x100:0x104] = b'MOD0'
    struct.pack_into('<iiiiii', text, 0x104, 0, 0, 0, hdr - 0x100, hdr + 0x40 - 0x100, 0)
    # a zR CIE (code alignment 4, data alignment -8, return register 30, FDE pointers pcrel sdata4), one FDE for the function, and the header's one (function, FDE) pair
    cie = struct.pack('<IIB', 0x14, 0, 1) + b'zR\0' + bytes([4, 0x78, 30, 1, 0x1B]) + bytes(7)
    ro[eh - RO:eh - RO + len(cie)] = cie
    fde = eh + len(cie)
    ro[fde - RO:fde - RO + 0x18] = struct.pack('<IIii', 0x14, fde + 4 - eh, FN - (fde + 8), 16) + bytes(8)
    struct.pack_into('<BBBBiIii', ro, 0, 1, 0x1B, 0x03, 0x3B, eh - (hdr + 4), 1, FN - hdr, fde - hdr)
    nso = bytearray(H)
    nso[0:4] = b'NSO0'
    for i, (mo, sz) in enumerate(((0, 0x2000), (RO, 0x1000), (DATA, 0x1000))):
        struct.pack_into('<III', nso, 0x10 + 0x10 * i, H + mo, mo, sz)
        struct.pack_into('<I', nso, 0x60 + 4 * i, sz)
    nso[0x40:0x50] = bytes.fromhex(BID)
    return bytes(nso + text + ro + data)


def synthetic_nro():
    """The same module as an NRO: the file is the image, with its header in the text's first 0x80 bytes (the branch and the MOD0 offset stay at +0 and +4)"""
    img = bytearray(synthetic_nso()[H:])
    img[0x10:0x14] = b'NRO0'
    struct.pack_into('<II', img, 0x18, len(img), 0)
    struct.pack_into('<6I', img, 0x20, 0, 0x2000, RO, 0x1000, DATA, 0x1000)
    img[0x40:0x50] = bytes.fromhex(BID)
    return bytes(img)


class Parse(unittest.TestCase):
    def test_ips_copy_and_rle_records(self):
        self.assertEqual(ep.parse_ips(ips([(0x300, b'\x01\x02'), (0x400, (3, 0xAA))])), [(0x300, b'\x01\x02'), (0x400, b'\xaa\xaa\xaa')])

    def test_ips32_takes_wide_offsets(self):
        self.assertEqual(ep.parse_ips(ips([(0x01000300, b'\x07')], wide=True)), [(0x01000300, b'\x07')])

    def test_a_cut_short_or_headerless_ips_is_an_error(self):
        with self.assertRaisesRegex(ep.Error, 'cut short'): ep.parse_ips(ips([(0x300, b'\x01\x02')])[:-2])
        with self.assertRaisesRegex(ep.Error, 'not an IPS'): ep.parse_ips(b'NOTIPS')

    def test_pchtxt_sections_shift_comments_and_strings(self):
        text = (f'@nsobid-{BID.upper()}000000\n\n// title\n@flag offset_shift 0x100\n\n'
                '@enabled\n00000200 1F2003D5 // nop\n00000300 "hi"\n\n'   # a blank line ends the section
                '@disabled\n00000400 AA\n\n'
                '@enabled\n00000500 AA BB\n\n'
                '@disabled\n00000600 CC\n@stop\n@enabled\n00000700 DD\n')
        self.assertEqual(ep.pchtxt_bid(text), BID)
        self.assertEqual(ep.parse_pchtxt(text), [(0x300, word(NOP)), (0x400, b'hi\0'), (0x600, b'\xaa\xbb')])

    def test_a_bad_pchtxt_record_is_an_error(self):
        with self.assertRaisesRegex(ep.Error, 'bad hex'): ep.parse_pchtxt('@nsobid-AA\n@enabled\n0010 XYZ\n')
        with self.assertRaisesRegex(ep.Error, 'bad offset'): ep.parse_pchtxt('@nsobid-AA\n@enabled\nZZ 00\n')
        with self.assertRaisesRegex(ep.Error, 'no bytes'): ep.parse_pchtxt('@nsobid-AA\n@enabled\n0010\n')


class Load(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.root = Path(self.tmp.name)

    def tearDown(self): self.tmp.cleanup()

    def put(self, rel, data):
        p = self.root / rel; p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data if isinstance(data, bytes) else data.encode())

    def load(self): return ep.load(str(self.root), BID + '0' * 32, 0x4000)

    def test_files_match_by_build_id_and_lose_the_header(self):
        self.put(f'60fps/{BID.upper()}{"0" * 32}.ips', ips([(H + FN, word(NOP))]))
        self.put(f'60fps/{BID}.ips', ips([(H + 0x2000, b'\x01')]))   # the short name matches too
        self.put('60fps/0123456789abcdef0123456789abcdef.ips', ips([(H + 0x10, b'\x09')]))   # another build
        self.put('mod/x.pchtxt', f'@nsobid-{BID}\n@flag offset_shift 0x100\n@enabled\n00000010 AA\n')
        self.put('mod/other.pchtxt', '@nsobid-0123456789abcdef\n@enabled\n00000010 BB\n')
        files, writes = self.load()
        self.assertEqual([w[:2] for w in writes], [(0x10, b'\xaa'), (FN, word(NOP)), (0x2000, b'\x01')])
        self.assertEqual(sorted(f[0] for f in files), sorted([f'60fps/{BID.upper()}{"0" * 32}.ips', f'60fps/{BID}.ips', 'mod/x.pchtxt']))

    def test_records_outside_the_module_are_errors(self):
        for name, recs, msg in (('header', [(0xF0, b'\0')], 'NSO header'), ('end', [(H + 0x3FFF, b'\0\0')], 'past the module')):
            with self.subTest(name):
                self.put(f'{BID}.ips', ips(recs))
                with self.assertRaisesRegex(ep.Error, msg): self.load()

    def test_overlapping_records_conflict_only_when_their_bytes_differ(self):
        self.put(f'a/{BID}.ips', ips([(H + 0x300, b'\x01\x02\x03')]))
        self.put(f'b/{BID}.ips', ips([(H + 0x301, b'\x02\x03\x04')]))   # the same bytes at 0x301 and 0x302
        self.assertEqual(len(self.load()[1]), 2)
        self.put(f'c/{BID}.ips', ips([(H + 0x302, b'\x99')]))
        with self.assertRaisesRegex(ep.Error, 'different bytes'): self.load()

    def test_nro_offsets_count_from_the_start(self):   # Atmosphère's ro patcher and Ryujinx's ApplyNroPatches: no header to subtract
        self.put(f'{BID}.ips', ips([(0x90, word(NOP)), (FN + 4, word(MOV_X1_X18))]))
        self.assertEqual([w[:2] for w in ep.load(str(self.root), BID, 0x4000, header=0)[1]], [(0x90, word(NOP)), (FN + 4, word(MOV_X1_X18))])

    def test_a_record_over_another_rewrite_is_an_error(self):
        writes = [(0x1FF, b'\0\0', 'p.ips')]
        ep.overlap(writes, [(0x201, 4, 'a cut')])
        with self.assertRaisesRegex(ep.Error, 'overlaps an observation gate at 0x200'): ep.overlap(writes, [(0x200, 4, 'an observation gate')])


@unittest.skipUnless(all(importlib.util.find_spec(m) for m in ('lz4', 'capstone')), 'needs lz4 and capstone')
class Rewriter(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.root = Path(self.tmp.name)
        for d in ('patches', 'out', 'games/X'): (self.root / d).mkdir(parents=True)
        (self.root / 'main.nso').write_bytes(synthetic_nso())

    def tearDown(self): self.tmp.cleanup()

    def rewrite(self, patches=True, module='main.nso'):
        env = {k: v for k, v in os.environ.items() if k not in ('DM_EXEFS_PATCHES', 'DM_GAME')} | {'DM_REWRITE_J': '1', 'DM_GAMES_DIR': str(self.root / 'games')}
        if patches: env['DM_EXEFS_PATCHES'] = str(self.root / 'patches')
        return subprocess.run([sys.executable, str(TOOLS / 'patch_nso.py'), str(self.root / module), str(self.root / 'out'), 'main'], capture_output=True, text=True, env=env)

    def put_ips(self, recs): (self.root / 'patches' / f'{BID}.ips').write_bytes(ips(recs))
    def out(self, ext): return (self.root / 'out' / f'main.{ext}').read_bytes()
    def sites(self): return {int(r[0], 16): r[1] for r in (l.split('\t') for l in self.out('sites.tsv').decode().splitlines())}
    def word_at(self, off): return struct.unpack_from('<I', self.out('patched.bin'), off)[0]

    def test_no_matching_patch_leaves_the_output_as_without_the_folder(self):
        p = self.rewrite(patches=False)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        before = self.out('patched.bin')
        p = self.rewrite()
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn('no ExeFS patch names this build', p.stdout)
        self.assertEqual(self.out('patched.bin'), before)

    def test_patched_x18_and_svc_words_are_rewritten(self):
        self.put_ips([(H + FN + 4, word(MOV_X1_X18)), (H + FN + 8, word(SVC_1))])
        p = self.rewrite()
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn(f'ExeFS patch {BID}.ips applied (2 records, 8 bytes)', p.stdout)
        self.assertEqual(self.sites(), {FN + 4: 'x18', FN + 8: 'svc'})
        for off in (FN + 4, FN + 8): self.assertEqual(self.word_at(off) >> 26, 0x05, 'a B into the patch area')
        self.assertIn(f'record\t{FN + 4:#x}\t0x4\t{BID}.ips\nrecord\t{FN + 8:#x}\t0x4\t{BID}.ips', self.out('exefs_patches.tsv').decode())
        self.assertIn(f'{FN + 4:#x}\t0x8', self.out('changed.tsv').decode())   # cheats may not write over the patched words
        # the same words in the file give the same output: a patch takes the rewriter's ordinary path
        patched, nso = self.out('patched.bin'), bytearray(synthetic_nso())
        nso[H + FN + 4:H + FN + 12] = word(MOV_X1_X18) + word(SVC_1)
        (self.root / 'main.nso').write_bytes(bytes(nso))
        self.assertEqual(self.rewrite(patches=False).returncode, 0)
        self.assertEqual(self.out('patched.bin'), patched)

    def test_a_patched_exclusive_load_gets_its_ordering(self):
        self.put_ips([(H + FN + 4, word(LDXR_W1_X2))])
        p = self.rewrite()
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(self.word_at(FN + 4), LDXR_W1_X2 | 0x8000, 'ldxr → ldaxr')

    def test_a_site_word_in_a_data_block_of_the_text_stops_the_rewrite(self):
        self.put_ips([(H + TEXT_DATA + 0x800, word(MOV_X1_X18))])
        p = self.rewrite()
        self.assertNotEqual(p.returncode, 0)
        self.assertIn('in a data block of the text, which is not rewritten', p.stderr)

    def test_the_same_word_in_ro_is_data(self):
        self.put_ips([(H + RO + 0x800, word(MOV_X1_X18))])
        p = self.rewrite()
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(self.word_at(RO + 0x800), MOV_X1_X18)

    def test_a_patch_in_the_header_stops_the_rewrite(self):
        self.put_ips([(0x20, word(NOP))])
        p = self.rewrite()
        self.assertNotEqual(p.returncode, 0)
        self.assertIn('inside the NSO header', p.stderr)

    def test_a_patch_over_a_registered_cut_stops_the_rewrite(self):
        (self.root / 'games/X/patches.py').write_text(f"CUTS = {{'{BID}': [({FN + 8:#x}, '{word(NOP).hex()}', '{word(RET).hex()}', 'test cut')]}}\n")
        self.put_ips([(H + FN + 8, word(MOV_X1_X18))])
        p = self.rewrite()
        self.assertNotEqual(p.returncode, 0)
        self.assertIn('overlaps a cut (test cut)', p.stderr)

    def test_a_patch_over_a_frame_step_site_names_the_patch(self):
        (self.root / 'games/X/patches.py').write_text(f"def stub(ctx):\n    ctx.site({FN + 8:#x}, '{word(NOP).hex()}'); ctx.put({FN + 8:#x}, [{RET:#x}], 'test step')\nFRAME_STEP = {{'{BID}': stub}}\n")
        self.put_ips([(H + FN + 8, word(MOV_X1_X18))])
        p = self.rewrite()
        self.assertNotEqual(p.returncode, 0)
        self.assertIn('overlaps a frame step site', p.stderr)
        self.assertNotIn('Traceback', p.stderr)

    def test_nro_records_are_not_shifted_by_a_header(self):
        (self.root / 'main.nro').write_bytes(synthetic_nro())
        self.put_ips([(0x90, word(NOP)), (FN + 4, word(MOV_X1_X18))])   # below 0x100 (an NSO's header) and in the function
        p = self.rewrite(module='main.nro')
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(self.sites(), {FN + 4: 'x18'})
        self.assertEqual(self.word_at(0x90), NOP)
        self.assertEqual(self.word_at(FN + 8), NOP, 'the word after the record is untouched')


if __name__ == '__main__':
    unittest.main()
