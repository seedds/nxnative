"""NSP script: the offline HTML pages of a version (extract_html) are read again when nx_extract, the packages or the options change, and reused otherwise.
A folder was reused for as long as it existed, so pages an older nx_extract had read wrongly stayed in every later build (Super Smash Bros. Ultimate 13.0.5's
How to Play page). No game files, keys, .NET or Xcode needed: nx_extract is a stand-in that writes one page."""
import importlib.util
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
if spec is None or spec.loader is None: raise ImportError('scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
APP = 0x0100AAAA00000000


class Pages(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        self.repo = self.tmp / 'repo'; (self.repo / 'dm/tools/nx_extract').mkdir(parents=True)
        (self.repo / 'dm/tools/nx_extract/Program.cs').write_text('// reads a patch alone\n')
        (self.repo / 'dm/tools/nx_extract/nx_extract.csproj').write_text('<Project/>\n')
        self.base, self.upd = self.tmp / 'base.nsp', self.tmp / 'update.nsp'
        self.base.write_bytes(b'b' * 10); self.upd.write_bytes(b'u' * 20)
        self.dst = self.tmp / 'extract/v65536'; self.dst.mkdir(parents=True)
        self.runs = []
        def run(cmd, log, **kw):   # nx_extract --html: a page that names the run that wrote it
            self.runs.append([str(c) for c in cmd])
            (Path(cmd[4]) / 'html0').mkdir(); (Path(cmd[4]) / 'html0/page.html').write_text(f'run {len(self.runs)}')
        for cm in (mock.patch.object(nsp, 'run', run), mock.patch.object(nsp, 'PROGRAM_INDEX', 0), mock.patch.object(nsp, 'ALLOW_DAMAGED', False)):
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def extract(self, upd=True):
        nsp.extract_html(self.tmp / 'nx_extract', self.tmp / 'prod.keys', self.dst, self.base, self.upd if upd else None, self.tmp, APP, self.repo)
        return (self.dst / 'html/html0/page.html').read_text()

    def test_the_same_inputs_reuse_the_pages(self):
        self.assertEqual(self.extract(), 'run 1')
        self.assertEqual(self.extract(), 'run 1')
        self.assertEqual(len(self.runs), 1)
        self.assertEqual(self.runs[0][3:6], [str(self.upd), str(self.dst / 'html.partial'), '--html'])

    def test_a_changed_nx_extract_reads_them_again(self):
        self.assertEqual(self.extract(), 'run 1')
        (self.repo / 'dm/tools/nx_extract/Program.cs').write_text('// reads a patch over the base game\'s\n')
        self.assertEqual(self.extract(), 'run 2')
        self.assertEqual(self.extract(), 'run 2')

    def test_pages_from_a_build_before_the_key_are_read_again_once(self):
        (self.dst / 'html/html0').mkdir(parents=True); (self.dst / 'html/html0/page.html').write_text('an older build, 1,641 files short')
        self.assertEqual(self.extract(), 'run 1')
        self.assertEqual(self.extract(), 'run 1')

    def test_another_package_or_option_reads_them_again(self):
        self.assertEqual(self.extract(), 'run 1')
        self.upd.write_bytes(b'u' * 21)                       # another update file
        self.assertEqual(self.extract(), 'run 2')
        self.assertEqual(self.extract(upd=False), 'run 3')    # the base game alone
        with mock.patch.object(nsp, 'ALLOW_DAMAGED', True): self.assertEqual(self.extract(upd=False), 'run 4')
        self.assertEqual(self.extract(upd=False), 'run 5')    # and back

    def test_a_failed_extraction_leaves_no_key(self):
        self.assertEqual(self.extract(), 'run 1')
        (self.repo / 'dm/tools/nx_extract/Program.cs').write_text('// another\n')
        def fail(cmd, log, **kw): raise nsp.BuildError('nx_extract exit code 4')
        with mock.patch.object(nsp, 'run', fail), self.assertRaises(nsp.BuildError): self.extract()
        self.assertFalse((self.dst / 'html.key').exists())
        self.assertEqual(self.extract(), 'run 2')             # the next build reads them: the old pages are not taken for this nx_extract's


if __name__ == '__main__':
    unittest.main()
