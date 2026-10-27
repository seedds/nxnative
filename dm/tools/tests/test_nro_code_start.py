"""An NRO's header is not code (GitHub issue LNPANDY/NX2Apple#14). An NRO's text segment starts at offset 0 with the file's header
in it: Hades' EOSSDK-Switch-Shipping.nro has text {0x0, 0xa41000} in its segment table and NRO0 at +0x10. patch_nso.py scanned the
text from 0, so on Final Fantasy X/X-2 HD Remaster the build ID at +0x40 decoded as "ldr x18, #0xfffffffffff40370" and the rewrite
stopped. code_start() gives 0x80 for an NRO and 0 for an NSO, and code_ranges() starts no range before it; both scans (the x18
words and the rewrite sites) walk those ranges. The functions are taken from patch_nso.py itself, which runs as a script."""
import ast
from pathlib import Path
import struct
import unittest

TOOLS = Path(__file__).resolve().parents[1]
tree = ast.parse((TOOLS / 'patch_nso.py').read_text(encoding='utf-8'))
scope = {}
exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in ('code_start', 'code_ranges')],
                        type_ignores=[]), str(TOOLS / 'patch_nso.py'), 'exec'), scope)
code_start, code_ranges = scope['code_start'], scope['code_ranges']

LDR_X18 = 0x58000000 | (0x7ff40370 >> 2 & 0x7ffff) << 5 | 18   # a literal load into x18: what the build ID's bytes read as


def nro(text_size=0x1000):
    """A synthetic NRO's first 0x80 bytes, laid out as Hades' EOS NRO is: text at offset 0, the header inside it, a build ID word that
    decodes as an x18 load"""
    h = bytearray(0x80)
    struct.pack_into('<I', h, 4, 0x1050)   # MOD0 offset
    h[0x10:0x14] = b'NRO0'
    struct.pack_into('<II', h, 0x20, 0, text_size)   # text {offset, size}
    struct.pack_into('<II', h, 0x28, text_size, 0x400)   # ro
    struct.pack_into('<II', h, 0x30, text_size + 0x400, 0x400)   # data
    struct.pack_into('<I', h, 0x44, LDR_X18)   # inside the build ID
    return bytes(h)


class CodeStart(unittest.TestCase):
    def test_an_nro_s_text_starts_at_zero_with_its_header_in_it(self):
        d = nro()
        self.assertEqual(struct.unpack_from('<II', d, 0x20), (0, 0x1000), 'the segment table gives the text offset 0, so it cannot bound the scan')
        self.assertEqual(code_start(d), 0x80)

    def test_an_nso_s_code_starts_at_zero(self):
        self.assertEqual(code_start(b'NSO0' + bytes(0x7c)), 0)

    def test_no_code_range_holds_the_header(self):
        ranges = code_ranges([(0x400, 0x500)], 0x1000, code_start(nro()))
        self.assertEqual(ranges, [(0x80, 0x400), (0x500, 0x1000)])
        self.assertFalse(any(s <= 0x44 < e for s, e in ranges), 'the build ID word that read as ldr x18 is outside every range')

    def test_a_data_block_over_the_start_and_an_nso_keep_their_ranges(self):
        self.assertEqual(code_ranges([(0x40, 0x200)], 0x1000, 0x80), [(0x200, 0x1000)])
        self.assertEqual(code_ranges([(0x400, 0x500)], 0x1000), [(0, 0x400), (0x500, 0x1000)], 'an NSO: as before')
        self.assertEqual(code_ranges([], 0x1000, 0), [(0, 0x1000)])


if __name__ == '__main__':
    unittest.main()
