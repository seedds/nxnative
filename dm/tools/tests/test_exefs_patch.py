#!/usr/bin/env python3
"""exefs_patch.py: the IPS / IPS32 / pchtxt readers (as dm/host/hle_exefs.c reads them), the rules for applying files whole, and which patched words are code
(docs/reference/rewriter.md, Code patches of mods rewritten with the module). Expectations are written by hand."""
import os, struct, sys, tempfile, unittest
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import exefs_patch as ep

try:
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
except ImportError:   # the classification needs capstone, as patch_nso.py does
    Cs = None

BID = 'E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348'


def w(*words): return b''.join(struct.pack('<I', x) for x in words)


class Readers(unittest.TestCase):
    def test_ips(self):
        d = b'PATCH' + b'\x00\x01\x10' + b'\x00\x02' + b'\xAA\xBB' + b'\x00\x01\x20' + b'\x00\x00' + b'\x00\x03' + b'\xCC' + b'\x00\x00\x10' + b'\x00\x01' + b'\xDD' + b'EOF'
        self.assertEqual(ep.parse_ips(d), [(0x10, b'\xAA\xBB'), (0x20, b'\xCC\xCC\xCC')])   # the record inside the header is dropped
        self.assertIsNone(ep.parse_ips(b'PATCH\x00\x01'))   # cut short
        self.assertEqual(ep.parse_ips(b'IPS32' + b'\x00\x00\x01\x04' + b'\x00\x01' + b'\x11' + b'EEOF'), [(4, b'\x11')])

    def test_pchtxt(self):
        t = f'@nsobid-{BID}\n@flag offset_shift 0x100\n// c\n@enabled\n00000010 AABB\n@little-endian\n00000020 11223344 // x\n00000030 "a\\n"\n@disabled\n00000040 FF\n@stop\n@enabled\n00000050 EE\n'
        bid, recs = ep.parse_pchtxt(t)
        self.assertEqual(bid, BID)
        self.assertEqual(recs, [(0x10, b'\xAA\xBB'), (0x20, b'\x44\x33\x22\x11'), (0x30, b'a\n')])
        self.assertIsNone(ep.parse_pchtxt('@enabled\n0010 ABC\n')[1])   # an odd number of digits
        self.assertTrue(ep.bid_match(BID[:16], BID.lower()[:32]))
        self.assertFalse(ep.bid_match(BID[:15], BID[:32]))

    def test_folder_rules_and_apply(self):
        with tempfile.TemporaryDirectory() as root:
            a, b, mod = (os.path.join(root, n) for n in ('a', 'b', 'withmodule'))
            for d in (a, b, mod): os.mkdir(d)
            open(os.path.join(a, 'p.pchtxt'), 'w').write(f'@nsobid-{BID}\n@flag offset_shift 0x100\n@enabled\n00000008 01020304\n')
            open(os.path.join(b, 'q.pchtxt'), 'w').write(f'@nsobid-{BID}\n@flag offset_shift 0x100\n@enabled\n00000008 05\n')   # another value under a's bytes
            open(os.path.join(b, 'other.pchtxt'), 'w').write('@nsobid-0123456789ABCDEF\n@enabled\n0 00\n')   # another build: not listed
            open(os.path.join(mod, 'subsdk9'), 'wb').write(b'x')
            open(os.path.join(mod, 'r.pchtxt'), 'w').write(f'@nsobid-{BID}\n@enabled\n00000110 00\n')
            files, skipped = ep.files_for([a, b, mod], BID[:32].lower())
            self.assertEqual([os.path.basename(p) for p, h, r in files], ['p.pchtxt', 'q.pchtxt'])
            self.assertEqual([p for p, why in skipped], [mod])
            img = bytearray(32)
            applied, skipped2, written = ep.apply(img, 32, 32, files, keep_out=[])
            self.assertEqual([os.path.basename(p) for p, h, r in applied], ['p.pchtxt'])
            self.assertIn('already patched differently', skipped2[0][1])
            self.assertEqual(bytes(img[8:12]), b'\x01\x02\x03\x04')
            self.assertEqual(written, {8, 9, 10, 11})
            img2 = bytearray(32)
            applied, skipped3, _ = ep.apply(img2, 32, 32, files[:1], keep_out=[(10, 4)])   # a registered cut there
            self.assertEqual(applied, []); self.assertIn('registered cut', skipped3[0][1])
            self.assertEqual(ep.fnv64(b''), 'cbf29ce484222325')


@unittest.skipIf(Cs is None, 'capstone is not installed')
class CodeWords(unittest.TestCase):
    def test_hook_reaches_code_not_data(self):
        # The module: one function, words 0..3 (mov x0, x1; nop; ret), padding (zeros) from 0x10 to 0x40.
        # The patch replaces word 0 (a hook site over mov x0, x1) with b 0x20, puts code at 0x20 (add w18, w18, #1; b 0x4) and, after it, a data chunk at 0x30 whose first word reads
        # as an x18 instruction (add w18, w18, #1) but is never branched to.
        orig = bytearray(w(0xAA0103E0, 0xD503201F, 0xD65F03C0, 0) + bytes(0x30))
        img = bytearray(orig)
        patch = {0x0: w(0x14000008), 0x20: w(0x11000652, 0x17FFFFF8), 0x30: w(0x11000652, 0xDEADBEEF)}
        written = set()
        for off, b in patch.items():
            img[off:off + len(b)] = b; written.update(range(off, off + len(b)))
        assert Cs is not None
        code, data = ep.code_words(img, bytes(orig), written, len(img), Cs(CS_ARCH_ARM64, CS_MODE_ARM))
        self.assertEqual(sorted(code), [0x0, 0x20, 0x24])
        self.assertEqual(sorted(data), [0x30, 0x34])
        self.assertEqual(ep.data_ranges(data), [(0x30, 0x38)])

    def test_function_entry_seed(self):
        # a function reached only through a pointer: pacibsp starts it
        orig = bytearray(0x20)
        img = bytearray(orig); body = w(0xD503237F, 0x11000652, 0xD65F0BFF)   # pacibsp; add w18, w18, #1; retab
        img[0x10:0x1C] = body
        assert Cs is not None
        code, data = ep.code_words(img, bytes(orig), set(range(0x10, 0x1C)), len(img), Cs(CS_ARCH_ARM64, CS_MODE_ARM))
        self.assertEqual(sorted(code), [0x10, 0x14, 0x18]); self.assertEqual(data, set())


if __name__ == '__main__':
    unittest.main()
