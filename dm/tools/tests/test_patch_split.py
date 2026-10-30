"""patch_nso.py's layout of the two patch areas of a text longer than a B reaches (two_split), on module sizes alone, without game files."""
import ast
from pathlib import Path
from typing import Any
import unittest

tree = ast.parse((Path(__file__).resolve().parents[1] / 'patch_nso.py').read_text())   # the program loads and rewrites an image: compile only the layout
ns: dict[str, Any] = {}
exec(compile(ast.Module([n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'two_split'], []), 'patch_nso.py', 'exec'), ns)
two_split = ns['two_split']

REACH, MARGIN = 0x8000000, 0x100000   # a B's reach; SPLIT's margin for a stub's exits past its site
def back_off(image_end): return ((((image_end + 0x3FFF) & ~0x3FFF) + 0x8000 + 0xFFFF) & ~0xFFFF)   # patch_nso.py's BACK_OFF: after the key page and the frame step page

class TwoSplit(unittest.TestCase):
    def reaches(self, image_end, front, split, back=0x400000):   # every site below SPLIT reaches the whole front area; every site above it, a back area of up to 4 MB
        self.assertLessEqual(split + MARGIN + front, REACH)
        self.assertLessEqual(back_off(image_end) + back - (split - MARGIN), REACH)

    def test_close_back_area_keeps_the_layout(self):   # EA SPORTS FC 27's main: BACK_OFF 0xdec0000 (docs/reference/rewriter.md)
        end = 0xdeb4000; self.assertEqual(back_off(end), 0xdec0000)
        self.assertEqual(two_split(end), (0x1000000, REACH - 0x1000000 - MARGIN))
        self.assertEqual(two_split(end, 0x1400000), (0x1400000, REACH - 0x1400000 - MARGIN))   # a rerun's size is kept
        self.reaches(end, *two_split(end))

    def test_far_back_area_moves_the_split(self):   # Bloodstained: Ritual of the Night 1.60's main: image 0xc8b8bc8, bss 0x271b438
        end = 0xc8b8bc8 + 0x271b438
        front, split = two_split(end)
        self.assertEqual((front, split), (0x620000, 0x76e0000))
        self.reaches(end, front, split)
        front, split = two_split(end, 0x700000)   # a rerun that asks for more front area still reaches both ways
        self.assertEqual(front, 0x700000)
        self.reaches(end, front, split)

    def test_no_split_reaches(self):
        self.assertIsInstance(two_split(0xf800000), str)   # the back area too far for any front area
        self.assertIsInstance(two_split(0xc8b8bc8 + 0x271b438, 0xb00000), str)   # a rerun's front area that crosses the back area's reach

if __name__ == '__main__':
    unittest.main()
