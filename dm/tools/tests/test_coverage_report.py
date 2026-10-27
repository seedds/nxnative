"""The coverage summary (dm/tools/coverage_report.py): llvm-cov and coverage.py JSON, the changed lines of a diff, diff coverage and the Markdown.
Synthetic samples and a temporary Git tree; no llvm, coverage.py, game files or keys needed.
"""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import coverage_report as cr

ROOT = '/work/nxnative'
# clang's segments for
#   1 int f(int x){
#   2  if(x>1)
#   3   return 2;
#   4  return 3;
#   5 }
# called once with x = 1 (llvm-cov show: 1, 1, 0, 1, 1), then a function skipped by the preprocessor on lines 7-8 and an unmapped gap on line 6
SEGMENTS = [[1, 13, 1, True, True, False], [2, 5, 1, True, True, False], [2, 8, 1, True, False, False], [2, 9, 0, True, False, True],
            [3, 3, 0, True, True, False], [3, 11, 1, True, False, False], [4, 2, 1, True, True, False], [4, 10, 1, True, False, False],
            [5, 2, 0, False, False, False], [7, 1, 0, False, True, False], [8, 9, 0, False, False, False]]
LLVM = {'type': 'llvm.coverage.json.export', 'version': '3.0.1', 'data': [{'files': [
    {'filename': ROOT + '/dm/host/../lib.c', 'segments': SEGMENTS, 'summary': {'lines': {'count': 5, 'covered': 4}}},
    {'filename': ROOT + '/dm/host/hle.h', 'segments': [], 'summary': {'lines': {'count': 0, 'covered': 0}}}]}]}
PY = {'meta': {'format': 3}, 'files': {
    'dm/tools/tool.py': {'executed_lines': [1, 2, 4], 'missing_lines': [5, 6], 'summary': {}},
    ROOT + '/scripts/run.py': {'executed_lines': [], 'missing_lines': [1, 2], 'summary': {}}}}
DIFF = '''diff --git a/dm/lib.c b/dm/lib.c
--- a/dm/lib.c
+++ b/dm/lib.c
@@ -2,0 +3,2 @@ int f(int x){
+  return 2;
+ return 3;
@@ -9 +10,0 @@
-gone
@@ -20 +20 @@
+changed
diff --git a/dm/old.py b/dm/old.py
deleted file mode 100644
--- a/dm/old.py
+++ /dev/null
@@ -1 +0,0 @@
-x = 1
diff --git a/dm/tools/tool.py b/dm/tools/tool.py
--- a/dm/tools/tool.py
+++ b/dm/tools/tool.py
@@ -1,0 +2 @@
+import os
@@ -5,0 +6,2 @@
+    y()
+
'''


class Parsing(unittest.TestCase):
    def test_llvm_lines_match_llvm_cov_show(self):
        self.assertEqual(cr.llvm_lines(SEGMENTS), {1: 1, 2: 1, 3: 0, 4: 1, 5: 1})
        self.assertEqual(cr.llvm_lines([]), {})

    def test_wrapped_region_counts_and_the_largest_start_wins(self):
        # line 2 lies inside the region of line 1; line 3 starts two regions, the larger count is the line's
        segs = [[1, 1, 3, True, True, False], [3, 1, 2, True, True, False], [3, 9, 7, True, True, False], [4, 1, 0, False, False, False]]
        self.assertEqual(cr.llvm_lines(segs), {1: 3, 2: 3, 3: 7, 4: 7})   # line 4 ends the region of 7

    def test_read_llvm_makes_paths_relative_and_merges_exports(self):
        doc = json.loads(json.dumps(LLVM))
        doc['data'].append({'files': [{'filename': ROOT + '/dm/lib.c', 'segments': [[3, 1, 5, True, True, False], [3, 9, 0, False, False, False]]}]})
        files = cr.read_llvm(doc, ROOT)
        self.assertEqual(sorted(files), ['dm/host/hle.h', 'dm/lib.c'])
        self.assertEqual(files['dm/lib.c'], {1: True, 2: True, 3: True, 4: True, 5: True})   # line 3 is run in the second export
        self.assertEqual(files['dm/host/hle.h'], {})

    def test_read_python(self):
        files = cr.read_python(PY, ROOT)
        self.assertEqual(files['dm/tools/tool.py'], {1: True, 2: True, 4: True, 5: False, 6: False})
        self.assertEqual(files['scripts/run.py'], {1: False, 2: False})


class Diff(unittest.TestCase):
    def test_changed_lines(self):
        self.assertEqual(cr.changed_lines(DIFF), {'dm/lib.c': {3, 4, 20}, 'dm/tools/tool.py': {2, 6, 7}})

    def test_diff_coverage_counts_only_known_lines(self):
        files = {'dm/lib.c': {3: False, 4: True}, 'dm/tools/tool.py': {2: True, 6: False}}
        changed = {'dm/lib.c': {3, 4, 20}, 'dm/tools/tool.py': {2, 6, 7}, 'dm/host/tests/t_x.c': {1}, 'dm/gfx/new.m': {5}, 'README.md': {1}}
        rows, unmeasured = cr.diff_coverage(changed, files)
        self.assertEqual(rows, {'dm/lib.c': ([4], [3]), 'dm/tools/tool.py': ([2], [6])})
        self.assertEqual(unmeasured, ['dm/gfx/new.m'])   # tests and non-sources are left out

    def test_ranges(self):
        self.assertEqual(cr.ranges([7, 1, 2, 3, 5, 6]), '1-3, 5-7')
        self.assertEqual(cr.ranges([4]), '4')
        self.assertEqual(cr.ranges([]), '')


class Markdown(unittest.TestCase):
    def test_areas_largest_files_and_diff(self):
        areas = [('Host', 'dm/host/tests', {'dm/a.c': {1: True, 2: False}, 'dm/b.c': {1: True}}), ('CPU textures', 'dm/gfx/tex/tests', None),
                 ('Python tools', 'dm/tools/tests', {'dm/tools/t.py': {1: True, 2: True, 3: False, 4: False}})]
        md = cr.markdown(areas, top=2, base='HEAD^1', diff={'dm/a.c': ([1], [2])}, unmeasured=['dm/c.m'])
        self.assertIn('Report only: this job never fails on coverage.', md)
        self.assertIn('| Host | dm/host/tests | 2 | 3 | 2 | 66.7% |', md)
        self.assertIn('| CPU textures | dm/gfx/tex/tests | | | | not measured |', md)
        self.assertIn('| Python tools | dm/tools/tests | 1 | 4 | 2 | 50.0% |', md)
        self.assertIn('### The 2 largest files', md)
        self.assertIn('| `dm/tools/t.py` | Python tools | 4 | 2 | 50.0% |', md)
        self.assertIn('| `dm/a.c` | Host | 2 | 1 | 50.0% |', md)
        self.assertNotIn('| `dm/b.c` |', md)
        self.assertIn('1 of 2 changed lines that the tests measure are run (50.0%).', md)
        self.assertIn('| `dm/a.c` | 1 | 1 | 50.0% | 2 |', md)
        self.assertIn('Changed sources no test measures: `dm/c.m`.', md)

    def test_no_diff_section_without_a_base_and_an_empty_diff(self):
        areas = [('Host', 'dm/host/tests', {})]
        self.assertNotIn('Diff coverage', cr.markdown(areas))
        self.assertIn('No changed line', cr.markdown(areas, base='main', diff={}))


class EndToEnd(unittest.TestCase):
    def test_main_against_a_base_revision(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            git = ['git', '-C', str(root), '-c', 'user.name=t', '-c', 'user.email=t@t', '-c', 'commit.gpgsign=false']
            subprocess.run(git[:3] + ['init', '-q'], check=True)
            src = root / 'dm/lib.c'; src.parent.mkdir(parents=True)
            src.write_text('int f(int x){\n if(x>1)\n return 3;\n}\n')
            subprocess.run(git + ['add', '.'], check=True); subprocess.run(git + ['commit', '-qm', 'a'], check=True)
            src.write_text('int f(int x){\n if(x>1)\n  return 2;\n return 3;\n}\n')
            subprocess.run(git + ['commit', '-qam', 'b'], check=True)
            doc = json.loads(json.dumps(LLVM).replace(ROOT, str(root)))
            (root / 'host.json').write_text(json.dumps(doc))
            out = io.StringIO()
            with contextlib.redirect_stdout(out): cr.main(['--host', str(root / 'host.json'), '--base', 'HEAD~1', '--root', str(root)])
            md = out.getvalue()
            self.assertIn('| Host | dm/host/tests | 2 | 5 | 4 | 80.0% |', md)
            self.assertIn('| Python tools | dm/tools/tests | | | | not measured |', md)
            self.assertIn('### Diff coverage against `HEAD~1`', md)
            self.assertIn('| `dm/lib.c` | 0 | 1 | 0.0% | 3 |', md)   # git adds line 3 (not run) before the unchanged " return 3;"
            self.assertNotIn('hle.h', md)   # no lines: not among the largest files


if __name__ == '__main__':
    unittest.main()
