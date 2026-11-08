"""dm/tools/patch_nso.py: the frame step state page (the page after the key page) stays free when the key page ends on a 64 KB boundary, where the appended patch area would start
right after it (Hades II 1.0.131248: key page 0x590c000, patch area 0x5910000, and the rewrite stopped with 'frame step state page'); modules without frame step stubs keep their layout"""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools/tests'))
from test_nso import make_nso

BUILD_ID = bytes(range(0x10)).hex()   # make_nso's build ID


def module(bss):
    """A main of 0x1000 of NOPs, an empty eh_frame_hdr in ro and 0x1000 of data: image 0x3000, then bss"""
    text = bytearray(struct.pack('<I', 0xd503201f) * 0x400)
    struct.pack_into('<I', text, 4, 0x10)   # MOD0 at +0x10: dynamic, bss start and end, eh_frame_hdr start and end (ro at 0x1000)
    struct.pack_into('<4siiiii', text, 0x10, b'MOD0', 0, 0x3000, 0x3000, 0x1000 - 0x10, 0x1000 - 0x10)
    ro = bytearray(0x1000); ro[:12] = bytes([1, 0x1b, 0x03, 0x3b]) + struct.pack('<iI', 0, 0)   # version 1, eh_frame_ptr, no FDEs
    return make_nso((bytes(text), bytes(ro), bytes(0x1000)), bss=bss)


def rewrite(bss, frame_step):
    """patch_nso.py on the module, with a FRAME_STEP of its build (no stubs) in an external profile when frame_step: {fixups key: value}"""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp); (root / 'games/G').mkdir(parents=True)
        if frame_step: (root / 'games/G/patches.py').write_text(f"FRAME_STEP = {{'{BUILD_ID}': lambda ctx: None}}\n")
        (root / 'main.nso').write_bytes(module(bss))
        r = subprocess.run([sys.executable, str(REPO / 'dm/tools/patch_nso.py'), str(root / 'main.nso'), str(root / 'out'), 'main'],
                           env=dict(os.environ, DM_GAMES_DIR=str(root / 'games'), DM_REWRITE_J='1'), capture_output=True, text=True)
        if r.returncode: raise AssertionError(r.stderr[-600:])
        rows = (root / 'out/main.fixups.tsv').read_text().splitlines()
        return {k[2:]: int(v, 0) for k, _, v in (l.partition('\t') for l in rows) if k in ('# PATCH_OFF', '# KEY_PAGE_OFF', '# FPS_STATE_OFF')}


class StatePage(unittest.TestCase):
    def test_key_page_ending_on_64_kb_moves_the_patch_area(self):
        f = rewrite(0x9000, True)   # key page 0xc000-0x10000
        self.assertEqual((f['KEY_PAGE_OFF'], f['FPS_STATE_OFF'], f['PATCH_OFF']), (0xc000, 0x10000, 0x20000))

    def test_room_left_by_the_rounding_keeps_the_layout(self):
        f = rewrite(0x5000, True)   # key page 0x8000-0xc000: the state page fits before 0x10000
        self.assertEqual((f['FPS_STATE_OFF'], f['PATCH_OFF']), (0xc000, 0x10000))

    def test_module_without_frame_step_keeps_the_layout(self):
        f = rewrite(0x9000, False)
        self.assertEqual((f['PATCH_OFF'], f.get('FPS_STATE_OFF')), (0x10000, None))


if __name__ == '__main__':
    unittest.main()
