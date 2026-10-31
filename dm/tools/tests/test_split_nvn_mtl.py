"""dm/tools/split_nvn_mtl.py: the cut of nvn_mtl.m puts back to the original byte for byte, on today's file, in a temporary folder."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
NVN = TOOLS.parents[1] / 'dm/gfx/nvn'
spec = importlib.util.spec_from_file_location('split_nvn_mtl', TOOLS / 'split_nvn_mtl.py')
assert spec and spec.loader
S = importlib.util.module_from_spec(spec)
spec.loader.exec_module(S)

def whole():
    """The unsplit nvn_mtl.m: the file itself, or the parts put back when the tree is split."""
    return S.read_tree(NVN)

class SplitNvnMtl(unittest.TestCase):
    def test_round_trip_in_a_temporary_folder(self):
        text = whole()
        plan = S.read_plan(S.PLAN)
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / 'nvn_mtl.m').write_text(text)
            main, parts, _new, _info = S.roundtrip(text, plan)
            (d / 'nvn_mtl.m').write_text(main)
            for p, t in parts.items(): (d / S.part_file(p)).write_text(t)
            self.assertEqual(S.read_tree(d), text)
            self.assertEqual(set(parts), set(S.PARTS))
            # cutting the joined folder again gives the same files
            main2, parts2, _n, _i = S.roundtrip(S.read_tree(d), plan)
            self.assertEqual((main2, parts2), (main, parts))

    def test_unbalanced_block_is_refused(self):
        with self.assertRaises(SystemExit): S.parse('static int f(void) {\n  return 1;\n')

if __name__ == '__main__':
    unittest.main()
