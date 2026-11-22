"""mod_layout: any NSO or NRO whose data shares the code's 16 KB page gets ro and data moved up in patch_nso.py, with every reference between text and the moved part following them
(FINAL FANTASY VIII Remastered's Math.nro: text, ro and data of 4 KB each). Images here are built by hand."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import mod_layout   # noqa: E402
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
    # a function reaching ro and data, a text adrp, an in-text literal load to an inline pool whose word mimics adrp into ro, and an unallocated 0xDC word
    pool = FN + 0x20
    code = [ADRP(0, FN, 0x2100), ldr_lit(1, FN + 4, LIT), adr(2, FN + 8, LIT), ADRP(3, FN + 12, FN),
            ldr_lit(4, FN + 16, pool), RET, 0xDC000000, 0, ADRP(5, pool, 0x2100), 0]
    struct.pack_into(f'<{len(code)}I', out, FN, *code)
    # MOD0 in ro: dynamic, bss start / end, eh_frame_hdr start / end, module object
    out[MOD0:MOD0 + 4] = b'MOD0'
    struct.pack_into('<6i', out, MOD0 + 4, DYN - MOD0, SIZE - MOD0, SIZE - MOD0, EHH - MOD0, EHH + 20 - MOD0, 0x2200 - MOD0)
    for i, (tag, v) in enumerate(((7, RELA), (8, 48), (9, 24), (6, SYMTAB), (5, STRTAB), (10, 12), (12, FN), (0, 0))):
        struct.pack_into('<qQ', out, DYN + 16 * i, tag, v)
    struct.pack_into('<QQq', out, RELA, 0x2100, 1027, FN)        # RELATIVE to text: stays
    struct.pack_into('<QQq', out, RELA + 24, 0x2108, 1027, LIT)  # RELATIVE to ro: moves
    struct.pack_into('<IBBHQQ', out, SYMTAB + 24, 1, 0x11, 0, 1, 0x2100, 8)   # data symbol: moves
    struct.pack_into('<IBBHQQ', out, SYMTAB + 48, 3, 0x12, 0, 1, FN, 24)      # function symbol: stays
    struct.pack_into('<IBBHQQ', out, SYMTAB + 72, 5, 0x16, 0, 1, 0x1800, 8)   # STT_TLS symbol (>= ro): must stay
    out[STRTAB:STRTAB + 12] = b'\0d\0f\0t\0\0\0\0\0\0'
    # .eh_frame_hdr (pcrel eh_frame pointer, datarel table) and one zR CIE + FDE for the function
    out[EHH:EHH + 4] = bytes((1, 0x1b, 3, 0x3b))
    struct.pack_into('<iI', out, EHH + 4, EHF - (EHH + 4), 1)
    struct.pack_into('<ii', out, EHH + 12, FN - EHH, EHF + 20 - EHH)
    struct.pack_into('<II', out, EHF, 16, 0)
    out[EHF + 8:EHF + 17] = b'\x01zR\0\x04\x78\x1e\x01\x1b'
    struct.pack_into('<IIiIB', out, EHF + 20, 16, EHF + 24 - EHF, FN - (EHF + 28), 24, 0)
    return out


def pack_nso(text, ro, data, ro_off=None, data_off=None, bss=0):
    """Build an uncompressed NSO0 file from text, ro and data segments."""
    ro_off = len(text) if ro_off is None else ro_off
    data_off = ro_off + len(ro) if data_off is None else data_off
    hdr = bytearray(0x100)
    hdr[:4] = b'NSO0'
    fo_t, fo_r, fo_d = 0x100, 0x100 + len(text), 0x100 + len(text) + len(ro)
    struct.pack_into('<III', hdr, 0x10, fo_t, 0, len(text))
    struct.pack_into('<III', hdr, 0x20, fo_r, ro_off, len(ro))
    struct.pack_into('<IIII', hdr, 0x30, fo_d, data_off, len(data), bss)
    struct.pack_into('<III', hdr, 0x60, len(text), len(ro), len(data))
    return bytes(hdr) + bytes(text) + bytes(ro) + bytes(data)


def small_nso():
    """An NSO with text=0x11000, ro=0x1000, data=0x1000 (data at 0x12000 < 0x14000), MOD0 in text, .eh_frame_hdr and DT_RELR."""
    tsz, ro, do, end = 0x11000, 0x11000, 0x12000, 0x13000
    m0, fn = 0x40, 0x100
    dyn, relr, symtab, strtab, ehh, ehf = do + 0x10, ro + 0x100, ro + 0x200, ro + 0x300, ro + 0x400, ro + 0x500
    slot0, slot2 = do + 0x200, do + 0x210
    img = bytearray(end)
    struct.pack_into('<I', img, 4, m0)
    img[m0:m0 + 4] = b'MOD0'
    struct.pack_into('<6i', img, m0 + 4, dyn - m0, end - m0, end + 0x1000 - m0, ehh - m0, ehh + 20 - m0, do + 0x300 - m0)
    struct.pack_into('<3I', img, fn, ADRP(0, fn, slot0), adr(1, fn + 4, ro + 0x20), RET)
    for i, (tag, v) in enumerate(((36, relr), (35, 16), (6, symtab), (5, strtab), (10, 8), (0, 0))):
        struct.pack_into('<qQ', img, dyn + 16 * i, tag, v)
    # DT_RELR: slot0 (start) + bitmap bit 2 -> slot0 + 16 = slot2
    struct.pack_into('<QQ', img, relr, slot0, (1 << 2) | 1)
    struct.pack_into('<Q', img, slot0, fn)          # points into text: stays
    struct.pack_into('<Q', img, slot2, ro + 0x20)   # points into ro: moves
    struct.pack_into('<IBBHQQ', img, symtab + 24, 1, 0x11, 0, 1, slot0, 8)
    img[strtab:strtab + 8] = b'\0g\0\0\0\0\0\0'
    img[ehh:ehh + 4] = bytes((1, 0x1b, 3, 0x3b))
    struct.pack_into('<iI', img, ehh + 4, ehf - (ehh + 4), 1)
    struct.pack_into('<ii', img, ehh + 12, fn - ehh, ehf + 20 - ehh)
    struct.pack_into('<II', img, ehf, 16, 0)
    img[ehf + 8:ehf + 17] = b'\x01zR\0\x04\x78\x1e\x01\x1b'
    struct.pack_into('<IIiIB', img, ehf + 20, 16, ehf + 24 - ehf, fn - (ehf + 28), 12, 0)
    return pack_nso(img[:tsz], img[ro:do], img[do:end], ro_off=ro, data_off=do, bss=0x1000), (tsz, ro, do, m0, fn, dyn, relr, slot0, slot2)


class Layout(unittest.TestCase):
    def setUp(self):
        self.old_bytes = bytes(math_nro())
        self.old_mod = nso.load(self.old_bytes, 'Math')
        self.new_mod, self.D, _ = mod_layout.relayout_module(self.old_mod)

    def test_data_on_its_own_16k_page(self):
        self.assertEqual(self.D, 0x2000)
        (t, tsz), (ro, _), (do, dsz) = self.new_mod.segments
        self.assertEqual((t, tsz, ro, do, dsz), (0, 0x1000, 0x3000, 0x4000, 0x1000))
        self.assertGreaterEqual(do, (tsz + 0x3FFF) & ~0x3FFF)
        self.assertEqual(self.new_mod.image[RO:RO + self.D], bytes(self.D))
        # The embedded NRO header stays self-consistent and loadable by nso.load()
        reloaded = nso.load(bytes(self.new_mod.image), 'Math')
        self.assertEqual(reloaded.segments, ((0, 0x3000), (0x3000, 0x1000), (0x4000, 0x1000)))
        self.assertEqual(mod_layout.relayout_module(self.new_mod)[1], 0)

    def test_code_references_and_inline_pool(self):
        D = self.D
        pool = FN + 0x20
        got = struct.unpack_from('<10I', self.new_mod.image, FN)
        self.assertEqual(got[:6], (ADRP(0, FN, 0x2100 + D), ldr_lit(1, FN + 4, LIT + D), adr(2, FN + 8, LIT + D),
                                   ADRP(3, FN + 12, FN), ldr_lit(4, FN + 16, pool), RET))
        self.assertEqual(got[6], 0xDC000000)              # unallocated opc=3 V=1 untouched
        self.assertEqual(got[8], ADRP(5, pool, 0x2100))   # inline literal pool word untouched

    def test_tables_and_tls_symbol(self):
        D, n = self.D, self.new_mod.image
        m0 = struct.unpack_from('<I', n, 4)[0]
        self.assertEqual(m0, MOD0 + D)
        self.assertEqual(struct.unpack_from('<i', n, m0 + 4)[0], DYN - MOD0)   # MOD0 and dynamic moved together
        tags = dict(struct.unpack_from('<qQ', n, DYN + D + 16 * i) for i in range(7))
        self.assertEqual((tags[7], tags[6], tags[5], tags[12]), (RELA + D, SYMTAB + D, STRTAB + D, FN))
        self.assertEqual(struct.unpack_from('<QQq', n, RELA + D), (0x2100 + D, 1027, FN))
        self.assertEqual(struct.unpack_from('<QQq', n, RELA + D + 24), (0x2108 + D, 1027, LIT + D))
        self.assertEqual(struct.unpack_from('<Q', n, SYMTAB + D + 24 + 8)[0], 0x2100 + D)
        self.assertEqual(struct.unpack_from('<Q', n, SYMTAB + D + 48 + 8)[0], FN)
        self.assertEqual(struct.unpack_from('<Q', n, SYMTAB + D + 72 + 8)[0], 0x1800)   # STT_TLS not shifted
        self.assertEqual(nso_code.fde_ranges(bytearray(n)), nso_code.fde_ranges(bytearray(self.old_bytes)))
        self.assertEqual(nso_code.fde_ranges(bytearray(n)), [(FN, FN + 24)])

    def test_nso_mod0_in_text_and_relr(self):
        raw, (tsz, ro, do, m0, fn, dyn, relr, slot0, slot2) = small_nso()
        mod = nso.load(raw, 'small')
        out, D, _ = mod_layout.relayout_module(mod)
        self.assertEqual(D, 0x2000)
        self.assertEqual(out.segments, ((0, tsz), (ro + D, 0x1000), (do + D, 0x1000)))
        self.assertEqual(struct.unpack_from('<I', out.image, 4)[0], m0)   # MOD0 in text stays
        self.assertEqual(struct.unpack_from('<i', out.image, m0 + 4)[0], dyn + D - m0)   # MOD0 -> dynamic shifts
        self.assertEqual(struct.unpack_from('<2I', out.image, fn), (ADRP(0, fn, slot0 + D), adr(1, fn + 4, ro + 0x20 + D)))
        self.assertEqual(struct.unpack_from('<QQ', out.image, relr + D), (slot0 + D, (1 << 2) | 1))
        self.assertEqual(struct.unpack_from('<Q', out.image, slot0 + D)[0], fn)
        self.assertEqual(struct.unpack_from('<Q', out.image, slot2 + D)[0], ro + 0x20 + D)

    def test_eh_frame_empty_aug_lsda_lpstart_and_omitted_table(self):
        d = bytearray(math_nro())
        lsda, cfi_magic = 0x1800, 0xCAFEBABE
        # Header: penc=0x1b (pcrel sdata4), cenc=0x0b (sdata4), tenc=0xff (omitted search table)
        d[EHH:EHH + 4] = bytes((1, 0x1b, 0x0b, 0xff))
        struct.pack_into('<ii', d, EHH + 4, EHF - (EHH + 4), 2)
        # CIE: zPLR (P=0x1b pcrel to FN, L=0x1b pcrel, R=0x1b pcrel)
        cie_body = bytearray(b'\x01zPLR\0\x04\x78\x1e')
        aug_data = bytearray([0x1b]) + struct.pack('<i', FN - (EHF + 8 + len(cie_body) + 2)) + bytes([0x1b, 0x1b])
        cie_body += bytes([len(aug_data)]) + aug_data
        while (len(cie_body) + 4) % 4: cie_body.append(0)
        struct.pack_into('<II', d, EHF, len(cie_body) + 4, 0)
        d[EHF + 8:EHF + 8 + len(cie_body)] = cie_body
        # FDE 1: aug_len=0 followed by CFI magic (must NOT be touched)
        fde1 = EHF + 8 + len(cie_body)
        struct.pack_into('<IIiIBI', d, fde1, 20, fde1 + 4 - EHF, FN - (fde1 + 8), 12, 0, cfi_magic)
        # FDE 2: aug_len=4 pointing to LSDA with lpe=0x1b (pcrel lpstart -> FN)
        fde2 = fde1 + 24
        struct.pack_into('<IIiIBi', d, fde2, 20, fde2 + 4 - EHF, FN + 12 - (fde2 + 8), 12, 4, lsda - (fde2 + 17))
        struct.pack_into('<I', d, fde2 + 24, 0)
        d[lsda] = 0x1b
        struct.pack_into('<i', d, lsda + 1, FN - (lsda + 1))

        out, D, _ = mod_layout.relayout_module(nso.load(bytes(d), 'eh'))
        self.assertEqual(D, 0x2000)
        # FDE 1 CFI magic preserved; FDE 1 pc_begin moved by -D; LSDA lpstart moved by -D
        self.assertEqual(struct.unpack_from('<I', out.image, fde1 + D + 17)[0], cfi_magic)
        self.assertEqual(struct.unpack_from('<i', out.image, fde1 + D + 8)[0], FN - (fde1 + D + 8))
        self.assertEqual(struct.unpack_from('<i', out.image, lsda + D + 1)[0], FN - (lsda + D + 1))

    def test_fitting_module_unchanged(self):
        ok = bytearray(0x5000); ok[0x10:0x14] = b'NRO0'
        struct.pack_into('<II', ok, 0x18, 0x5000, 0)
        struct.pack_into('<6I', ok, 0x20, 0, 0x2000, 0x2000, 0x2000, 0x4000, 0x1000)
        mod = nso.load(bytes(ok), 'ok')
        self.assertEqual(mod_layout.relayout_module(mod), (mod, 0, []))

    def test_patch_nso_end_to_end_nro_and_nso(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        nro_path = root / 'nro0.nro'; nro_path.write_bytes(self.old_bytes)
        nso_raw, _ = small_nso()
        nso_path = root / 'subsdk9'; nso_path.write_bytes(nso_raw)
        env = dict(os.environ, DM_GAME='01008B900DC0A000', DM_REWRITE_J='1')
        for path, name, exp_te, exp_ro, exp_do in ((nro_path, 'nro0', 0x1000, 0x3000, 0x4000),
                                                   (nso_path, 'subsdk9', 0x11000, 0x13000, 0x14000)):
            out_dir = root / f'out_{name}'; out_dir.mkdir()
            p = subprocess.run([sys.executable, str(TOOLS / 'patch_nso.py'), str(path), str(out_dir), name],
                               env=env, capture_output=True, text=True)
            self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
            hdr = dict(ln[2:].split() for ln in (out_dir / f'{name}.fixups.tsv').read_text().splitlines() if ln.startswith('# '))
            self.assertEqual((int(hdr['TEXT_END'], 16), int(hdr['RO_OFF'], 16), int(hdr['DATA_OFF'], 16)), (exp_te, exp_ro, exp_do))
            self.assertGreaterEqual(exp_do, (exp_te + 0x3FFF) & ~0x3FFF)
            if name == 'nro0':
                self.assertEqual(nro_path.read_bytes(), self.old_bytes)   # on-disk NRO untouched
                patched = (out_dir / 'nro0.patched.bin').read_bytes()
                self.assertEqual(struct.unpack_from('<I', patched, 0x30)[0], exp_do)


if __name__ == '__main__': unittest.main()
