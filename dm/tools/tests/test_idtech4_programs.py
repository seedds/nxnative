"""idtech4_programs.py: id Tech 4's render program finders found from the renderer's code in a synthetic module laid out as DOOM 3's main is (the loader by its GLSL cache path,
the per-stage loaders by the stage they pass, the finders as the callers of one of them, the manager slot from the material parser's load), and the render programs of a .resources
container. No game files."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(REPO / 'dm/tools'), str(REPO / 'dm/gfx/tools')]
import idtech4_programs as ip   # noqa: E402

LOADER, LV, LF, INIT, FINDV, FINDF, PARSER, END = 0x100, 0x140, 0x180, 0x1c0, 0x200, 0x240, 0x280, 0x2c0
MOD0, EHS, STRING, SLOT = 0x2000, 0x2100, 0x3001, 0x4018


def adrp(pc, rd, target):
    imm = ((target & ~0xFFF) - (pc & ~0xFFF)) >> 12
    return 0x90000000 | ((imm & 3) << 29) | (((imm >> 2) & 0x7FFFF) << 5) | rd
def add(rd, rn, imm): return 0x91000000 | (imm << 10) | (rn << 5) | rd
def movz_w1(imm): return 0x52800001 | (imm << 5)
def bl(pc, t): return 0x94000000 | (((t - pc) >> 2) & 0x3FFFFFF)
def ldr_x0(off): return 0xF9400000 | ((off // 8) << 10)


def image(cache_path=ip.CACHE_PATH, stages=(0x8b31, 0x8b30)):
    img = bytearray(0x5000)
    code = {LOADER: [adrp(LOADER, 1, STRING), add(1, 1, STRING & 0xFFF)],
            LV: [movz_w1(stages[0]), bl(LV + 4, LOADER)], LF: [movz_w1(stages[1]), bl(LF + 4, LOADER)],
            INIT: [bl(INIT, LV), bl(INIT + 4, LF)], FINDV: [bl(FINDV, LV)], FINDF: [bl(FINDF, LF)],
            PARSER: [adrp(PARSER, 0, SLOT), ldr_x0(SLOT & 0xFFF), bl(PARSER + 8, FINDV), adrp(PARSER + 12, 0, SLOT), ldr_x0(SLOT & 0xFFF), bl(PARSER + 20, FINDF)]}
    for at, ws in code.items():
        for i, w in enumerate(ws): struct.pack_into('<I', img, at + 4 * i, w)
    struct.pack_into('<I', img, 4, MOD0); img[MOD0:MOD0 + 4] = b'MOD0'; struct.pack_into('<i', img, MOD0 + 16, EHS - MOD0)
    fns = [LOADER, LV, LF, INIT, FINDV, FINDF, PARSER, END]
    struct.pack_into('<i', img, EHS + 8, len(fns))
    for i, f in enumerate(fns): struct.pack_into('<ii', img, EHS + 12 + 8 * i, f - EHS, 0)
    img[STRING:STRING + len(cache_path)] = cache_path
    return bytes(img), (0, 0x300)


class Find(unittest.TestCase):
    def test_finders_and_manager(self):
        self.assertEqual(ip.find(*image()), {'find_vertex': FINDV, 'find_fragment': FINDF, 'manager_slot': SLOT})

    def test_stages_swapped(self):   # the vertex finder is the one over the loader that passes GL_VERTEX_SHADER, wherever it is
        self.assertEqual(ip.find(*image(stages=(0x8b30, 0x8b31))), {'find_vertex': FINDF, 'find_fragment': FINDV, 'manager_slot': SLOT})

    def test_another_engine(self):
        self.assertIsNone(ip.find(*image(cache_path=b'shaders\\glsl\\%s\0')))
        self.assertIsNone(ip.find(*image(stages=(0x8b31, 0x8b31))))


class Containers(unittest.TestCase):
    def test_render_programs_of_the_containers(self):
        names = ['renderprogs/interaction.vertex', 'renderprogs/interaction.pixel', 'renderprogs/shadow.vertex', 'renderprogs/global.inc',
                 'renderprogs/hlsl/interaction.v360', 'materials/doom.mtr']
        table = struct.pack('>I', len(names)) + b''.join(struct.pack('<I', len(n)) + n.encode() + struct.pack('>II', 0, 0) for n in names)
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / 'base').mkdir()
            (Path(d) / 'base/_common.resources').write_bytes(struct.pack('>III', 0xD000000D, 12, len(table)) + table)
            (Path(d) / 'base/other.resources').write_bytes(struct.pack('>III', 0x12345678, 12, 0))   # not a container
            self.assertEqual(ip.programs(d), (['interaction', 'shadow'], ['interaction']))


if __name__ == '__main__':
    unittest.main()
