"""nro_layout: a small NRO whose data shares the code's 16 KB page gets ro and data moved up, with every reference between text and the moved part following them
(FINAL FANTASY VIII Remastered's Math.nro: text, ro and data of 4 KB each). The image here is built by hand in the same shape."""
from pathlib import Path
import struct
import sys
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import nro_layout   # noqa: E402
import nso   # noqa: E402
import nso_code   # noqa: E402
from nso_asm import ADRP   # noqa: E402

RET = 0xd65f03c0
RO, DATA, SIZE = 0x1000, 0x2000, 0x3000
MOD0, DYN, EHH, EHF, RELA, SYMTAB, STRTAB, LIT = 0x1040, 0x2008, 0x1600, 0x1700, 0x1100, 0x1200, 0x1300, 0x1500
FN = 0x80


def adr(d, pc, to): imm = (to - pc) & 0x1FFFFF; return 0x10000000 | (imm & 3) << 29 | (imm >> 2) << 5 | d
def ldr_lit(t, pc, to): return 0x58000000 | (((to - pc) >> 2) & 0x7FFFF) << 5 | t


def math_nro():
    out = bytearray(SIZE)
    out[0x10:0x14] = b'NRO0'
    struct.pack_into('<I', out, 4, MOD0)
    struct.pack_into('<II', out, 0x18, SIZE, 0)
    struct.pack_into('<6I', out, 0x20, 0, RO, RO, DATA - RO, DATA, SIZE - DATA)
    # a function that reaches ro and data, and one text address taken by adrp
    code = [ADRP(0, FN, 0x2100), ldr_lit(1, FN + 4, LIT), adr(2, FN + 8, LIT), ADRP(3, FN + 12, FN), RET]
    struct.pack_into('<5I', out, FN, *code)
    # MOD0 in ro: dynamic, bss start / end, eh_frame_hdr start / end, module object
    out[MOD0:MOD0 + 4] = b'MOD0'
    struct.pack_into('<6i', out, MOD0 + 4, DYN - MOD0, SIZE - MOD0, SIZE - MOD0, EHH - MOD0, EHH + 20 - MOD0, 0x2200 - MOD0)
    for i, (tag, v) in enumerate(((7, RELA), (8, 48), (9, 24), (6, SYMTAB), (5, STRTAB), (10, 8), (12, FN), (0, 0))):
        struct.pack_into('<qQ', out, DYN + 16 * i, tag, v)
    struct.pack_into('<QQq', out, RELA, 0x2100, 1027, FN)        # RELATIVE to text: stays
    struct.pack_into('<QQq', out, RELA + 24, 0x2108, 1027, LIT)  # RELATIVE to ro: moves
    struct.pack_into('<IBBHQQ', out, SYMTAB + 24, 1, 0x11, 0, 1, 0x2100, 8)   # a data symbol
    struct.pack_into('<IBBHQQ', out, SYMTAB + 48, 3, 0x12, 0, 1, FN, 20)      # a function
    out[STRTAB:STRTAB + 8] = b'\0d\0f\0\0\0\0'
    # .eh_frame_hdr (pcrel eh_frame pointer, datarel table) and one zR CIE + FDE for the function
    out[EHH:EHH + 4] = bytes((1, 0x1b, 3, 0x3b))
    struct.pack_into('<iI', out, EHH + 4, EHF - (EHH + 4), 1)
    struct.pack_into('<ii', out, EHH + 12, FN - EHH, EHF + 20 - EHH)
    struct.pack_into('<II', out, EHF, 16, 0)
    out[EHF + 8:EHF + 17] = b'\x01zR\0\x04\x78\x1e\x01\x1b'
    struct.pack_into('<IIiIB', out, EHF + 20, 16, EHF + 24 - EHF, FN - (EHF + 28), 20, 0)
    return out


class Layout(unittest.TestCase):
    def setUp(self):
        self.old = bytes(math_nro())
        self.new, self.D, _ = nro_layout.relayout(self.old)

    def test_data_on_its_own_16k_page(self):
        self.assertEqual(self.D, 0x2000)
        m = nso.load(self.new, 'Math')   # the loader's own checks: segments back to back
        (t, tsz), (ro, _), (do, dsz) = m.segments
        self.assertEqual((t, tsz, ro, do, dsz), (0, 0x3000, 0x3000, 0x4000, 0x1000))
        self.assertGreaterEqual(do, (tsz + 0x3FFF) & ~0x3FFF)
        self.assertEqual(self.new[RO:RO + self.D], bytes(self.D))
        self.assertEqual(nro_layout.relayout(self.new)[1], 0)   # once is enough

    def test_code_references_follow(self):
        D = self.D
        self.assertEqual(struct.unpack_from('<5I', self.new, FN),
                         (ADRP(0, FN, 0x2100 + D), ldr_lit(1, FN + 4, LIT + D), adr(2, FN + 8, LIT + D), ADRP(3, FN + 12, FN), RET))

    def test_tables_follow(self):
        D, n = self.D, self.new
        m0 = struct.unpack_from('<I', n, 4)[0]
        self.assertEqual(m0, MOD0 + D)
        self.assertEqual(struct.unpack_from('<i', n, m0 + 4)[0], DYN - MOD0)   # MOD0 and dynamic moved together
        tags = dict(struct.unpack_from('<qQ', n, DYN + D + 16 * i) for i in range(7))
        self.assertEqual((tags[7], tags[6], tags[5], tags[12]), (RELA + D, SYMTAB + D, STRTAB + D, FN))
        self.assertEqual(struct.unpack_from('<QQq', n, RELA + D), (0x2100 + D, 1027, FN))
        self.assertEqual(struct.unpack_from('<QQq', n, RELA + D + 24), (0x2108 + D, 1027, LIT + D))
        self.assertEqual(struct.unpack_from('<Q', n, SYMTAB + D + 24 + 8)[0], 0x2100 + D)
        self.assertEqual(struct.unpack_from('<Q', n, SYMTAB + D + 48 + 8)[0], FN)
        self.assertEqual(nso_code.fde_ranges(bytearray(n)), nso_code.fde_ranges(bytearray(self.old)))
        self.assertEqual(nso_code.fde_ranges(bytearray(n)), [(FN, FN + 20)])

    def test_fitting_module_unchanged(self):
        ok = bytearray(0x5000); ok[0x10:0x14] = b'NRO0'
        struct.pack_into('<6I', ok, 0x20, 0, 0x2000, 0x2000, 0x2000, 0x4000, 0x1000)
        self.assertEqual(nro_layout.relayout(bytes(ok)), (bytes(ok), 0, []))


if __name__ == '__main__': unittest.main()
