"""Tests for devkitA64-style .eh_frame-only NSOs (patch_nso.py, mod_layout.py) and scripts/prepare_mod.py."""
from __future__ import annotations

import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
TOOLS = REPO / 'dm/tools'
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(TOOLS))

import mod_layout   # noqa: E402
import nso   # noqa: E402
from nso_asm import ADRP   # noqa: E402
from scripts import prepare_mod   # noqa: E402

RET = 0xD65F03C0
SVC_TICK = 0xD40003C1   # svc #0x1e
NOP = 0xD503201F


def pack_nso(text: bytes, ro: bytes, data: bytes, ro_off: int, data_off: int, bss: int = 0) -> bytes:
    hdr = bytearray(0x100)
    hdr[:4] = b'NSO0'
    fo_t = 0x100
    fo_r = fo_t + len(text)
    fo_d = fo_r + len(ro)
    struct.pack_into('<III', hdr, 0x10, fo_t, 0, len(text))
    struct.pack_into('<III', hdr, 0x20, fo_r, ro_off, len(ro))
    struct.pack_into('<IIII', hdr, 0x30, fo_d, data_off, len(data), bss)
    struct.pack_into('<III', hdr, 0x60, len(text), len(ro), len(data))
    hdr[0x40:0x50] = bytes.fromhex('112233445566778899aabbccddeeff00')
    return bytes(hdr) + text + ro + data


def devkita64_nso(small_relayout: bool = False, no_eh: bool = False) -> bytes:
    """Build a minimal devkitA64-style NSO with empty MOD0 eh_frame_hdr range pointing directly at .eh_frame."""
    if small_relayout:
        tsz, ro, do, end = 0x1000, 0x1000, 0x2000, 0x3000
    else:
        tsz, ro, do, end = 0x4000, 0x4000, 0x5000, 0x6000
    m0 = 0x40
    fn1, fn2 = 0x100, 0x120
    dyn = do + 0x10
    symtab, strtab = ro + 0x100, ro + 0x200
    ehf = ro + 0x300
    pers_slot = do + 0x200

    img = bytearray(end)
    struct.pack_into('<I', img, 4, m0)
    img[m0:m0 + 4] = b'MOD0'
    eh_rel = 0 if no_eh else (ehf - m0)
    struct.pack_into('<6i', img, m0 + 4, dyn - m0, end - m0, end - m0, eh_rel, eh_rel, do + 0x300 - m0)

    # fn1: adrp x0, pers_slot; svc #0x1e; ret; fn2: nop; ret
    struct.pack_into('<3I', img, fn1, ADRP(0, fn1, pers_slot), SVC_TICK, RET)
    struct.pack_into('<2I', img, fn2, NOP, RET)

    for i, (tag, v) in enumerate(((6, symtab), (5, strtab), (10, 8), (12, fn1), (0, 0))):
        struct.pack_into('<qQ', img, dyn + 16 * i, tag, v)
    struct.pack_into('<IBBHQQ', img, symtab + 24, 1, 0x12, 0, 1, fn1, 12)
    img[strtab:strtab + 8] = b'\0f\0\0\0\0\0\0'

    if not no_eh:
        # CIE 0 (version 3, augmentation "zPR", personality pointer into data + pcrel sdata4 FDE encoding)
        cie0 = ehf
        pers_pos = cie0 + 8 + 1 + 4 + 3 + 1 + 1   # ver(1) + "zPR\0"(4) + align/ret(3) + auglen(1) + P_enc(1)
        cie_body = bytearray(b'\x03zPR\0\x04\x78\x1e')
        aug_data = bytearray([0x1B]) + struct.pack('<i', pers_slot - pers_pos) + bytes([0x1B])
        cie_body.append(len(aug_data))
        cie_body.extend(aug_data)
        while (len(cie_body) + 4) % 4:
            cie_body.append(0)
        struct.pack_into('<II', img, cie0, len(cie_body) + 4, 0)
        img[cie0 + 8:cie0 + 8 + len(cie_body)] = cie_body

        fde1 = cie0 + 8 + len(cie_body)
        struct.pack_into('<IIiIB', img, fde1, 16, fde1 + 4 - cie0, fn1 - (fde1 + 8), 12, 0)
        fde2 = fde1 + 20
        struct.pack_into('<IIiIB', img, fde2, 16, fde2 + 4 - cie0, fn2 - (fde2 + 8), 8, 0)
        # Zero terminator follows naturally

    return pack_nso(bytes(img[:tsz]), bytes(img[ro:do]), bytes(img[do:end]), ro_off=ro, data_off=do)


class DevkitA64EhFrame(unittest.TestCase):
    def test_mod_layout_updates_raw_eh_frame_without_hdr(self):
        raw = devkita64_nso(small_relayout=True)
        mod = nso.load(raw, 'subsdk9')
        out, d, _ = mod_layout.relayout_module(mod)
        self.assertEqual(d, 0x2000)
        # Personality pointer in raw .eh_frame pointed from ro (0x1300+...) to pers_slot (0x2200 -> 0x4200)
        ehf = 0x1300
        pers_pos = ehf + 8 + 1 + 4 + 3 + 1 + 1
        old_rel = struct.unpack_from('<i', raw[0x100:], pers_pos)[0]
        new_rel = struct.unpack_from('<i', out.image, pers_pos)[0]
        self.assertEqual(new_rel, old_rel + d)

    def test_patch_nso_rewrites_raw_eh_frame_and_no_eh_nso(self):
        for name, raw in (('subsdk9', devkita64_nso()), ('subsdk8', devkita64_nso(no_eh=True))):
            with tempfile.TemporaryDirectory() as td:
                src = Path(td) / name
                out = Path(td) / 'out'
                out.mkdir()
                src.write_bytes(raw)
                env = {k: v for k, v in os.environ.items() if k != 'DM_GAME'}
                env['DM_REWRITE_J'] = '1'
                r = subprocess.run(
                    [sys.executable, str(TOOLS / 'patch_nso.py'), str(src), str(out), name],
                    env=env, capture_output=True, text=True, check=False,
                )
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                self.assertTrue((out / f'{name}.patched.bin').is_file())
                self.assertTrue((out / f'{name}.fixups.tsv').is_file())
                self.assertTrue((out / f'{name}.changed.tsv').is_file())
                gates = (out / f'{name}.gates.tsv').read_text()
                self.assertIn('\tsvc\t30\t', gates)


class PrepareModScript(unittest.TestCase):
    def test_find_plugins_sorted_and_filtered(self):
        with tempfile.TemporaryDirectory() as td:
            exefs = Path(td) / 'exefs'
            exefs.mkdir()
            for nm in ('subsdk10', 'subsdk2', 'subsdk9', 'subsdk9.bak', 'main', 'notes.txt'):
                (exefs / nm).write_bytes(b'NSO0')
            (exefs / 'subsdk3').mkdir()
            names = [p.name for p in prepare_mod.find_plugins(exefs)]
            self.assertEqual(names, ['subsdk2', 'subsdk9', 'subsdk10'])

    def test_prepare_mod_end_to_end_and_rejects_non_nso(self):
        with tempfile.TemporaryDirectory() as td:
            mod = Path(td) / 'compass'
            exefs = mod / 'exefs'
            exefs.mkdir(parents=True)
            (exefs / 'subsdk9').write_bytes(devkita64_nso())
            (exefs / 'main').write_bytes(b'NSO0')
            old_j = os.environ.get('DM_REWRITE_J')
            os.environ['DM_REWRITE_J'] = '1'
            try:
                count = prepare_mod.prepare_mod(mod)
            finally:
                if old_j is None:
                    os.environ.pop('DM_REWRITE_J', None)
                else:
                    os.environ['DM_REWRITE_J'] = old_j
            self.assertEqual(count, 1)
            self.assertTrue((exefs / 'nxnative/subsdk9.patched.bin').is_file())
            self.assertTrue((exefs / 'nxnative/subsdk9.changed.tsv').is_file())

            bad = Path(td) / 'badmod'
            (bad / 'exefs').mkdir(parents=True)
            (bad / 'exefs/subsdk9').write_bytes(b'NOT_NSO')
            with self.assertRaises(ValueError):
                prepare_mod.prepare_mod(bad)


if __name__ == '__main__':
    unittest.main()
