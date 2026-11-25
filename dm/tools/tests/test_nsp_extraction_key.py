"""NSP script: what nx_extract wrote for a version, its program (extract: ExeFS/ and RomFS.img) and its offline HTML pages (extract_html), is extracted again when nx_extract,
the packages (name, size, time) or the options change, and reused otherwise (extraction_key). Both were reused for as long as they existed, so pages an older nx_extract had read
wrongly stayed in every later build (Super Smash Bros. Ultimate 13.0.5's How to Play page, PR #1545), and so did an image packed with --allow-damaged. No game files, keys, .NET or
Xcode needed: nx_extract is a stand-in that writes one page, or an ExeFS main and an image. The image is reused for its key and a valid index, whatever it holds: the profile's
RomFS marker was checked too, but that is the registered profile's, so another edition's files (Persona 5 Royal's Asian title: CPK/ALL_ASIA.CPK, no CPK/ALL_USEU.CPK) were
extracted again on every build."""
import importlib.util
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
import romfs_image  # noqa: E402
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
if spec is None or spec.loader is None: raise ImportError('scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
APP = 0x0100AAAA00000000


class Inputs(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        self.repo = self.tmp / 'repo'; (self.repo / 'dm/tools/nx_extract').mkdir(parents=True)
        (self.repo / 'dm/tools/nx_extract/Program.cs').write_text('// reads a patch alone\n')
        (self.repo / 'dm/tools/nx_extract/nx_extract.csproj').write_text('<Project/>\n')
        (self.repo / 'dm/tools/nx_extract/NxTree.cs').write_text('// reads NX archives\n')
        self.base, self.upd = self.tmp / 'base.nsp', self.tmp / 'update.nsp'
        self.base.write_bytes(b'b' * 10); self.upd.write_bytes(b'u' * 20)
        self.ex = self.tmp / 'extract'; self.ex.mkdir()
        self.dst = self.ex / 'v65536'
        self.runs = []
        def run(cmd, log, **kw):   # nx_extract: what it writes names the run that wrote it
            self.runs.append([str(c) for c in cmd]); out, n = Path(cmd[4]), len(self.runs)
            if cmd[5] == '--html': (out / 'html0').mkdir(); (out / 'html0/page.html').write_text(f'run {n}')
            else:   # another edition's RomFS: none of a registered profile's files
                (out / 'ExeFS').mkdir(parents=True); (out / 'ExeFS/main').write_text(f'run {n}')
                rom = self.tmp / f'romfs{n}'; (rom / 'CPK').mkdir(parents=True); (rom / 'CPK/ALL_ASIA.CPK').write_text(f'run {n}')
                romfs_image.pack(rom, out / 'RomFS.img')
        for cm in (mock.patch.object(nsp, 'run', run), mock.patch.object(nsp, 'PROGRAM_INDEX', 0), mock.patch.object(nsp, 'ALLOW_DAMAGED', False),
                   mock.patch.object(nsp, 'say', lambda *a, **k: None)):
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def program(self, upd=True):
        d = nsp.extract(self.tmp / 'nx_extract', self.tmp / 'prod.keys', self.ex, self.dst.name, self.base, self.upd if upd else None, self.tmp, APP, self.repo)
        return (nsp.rfs.source(d) / 'CPK/ALL_ASIA.CPK').read_text()

    def pages(self, upd=True):
        self.dst.mkdir(exist_ok=True)
        nsp.extract_html(self.tmp / 'nx_extract', self.tmp / 'prod.keys', self.dst, self.base, self.upd if upd else None, self.tmp, APP, self.repo)
        return (self.dst / 'html/html0/page.html').read_text()

    def change_nx_extract(self, text):
        (self.repo / 'dm/tools/nx_extract/Program.cs').write_text(text)


class Pages(Inputs):
    def test_the_same_inputs_reuse_the_pages(self):
        self.assertEqual(self.pages(), 'run 1')
        self.assertEqual(self.pages(), 'run 1')
        self.assertEqual(len(self.runs), 1)
        self.assertEqual(self.runs[0][3:6], [str(self.upd), str(self.dst / 'html.partial'), '--html'])

    def test_a_changed_nx_extract_reads_them_again(self):
        self.assertEqual(self.pages(), 'run 1')
        self.change_nx_extract('// reads a patch over the base game\'s\n')
        self.assertEqual(self.pages(), 'run 2')
        self.assertEqual(self.pages(), 'run 2')

    def test_pages_from_a_build_before_the_key_are_read_again_once(self):
        (self.dst / 'html/html0').mkdir(parents=True); (self.dst / 'html/html0/page.html').write_text('an older build, 1,641 files short')
        self.assertEqual(self.pages(), 'run 1')
        self.assertEqual(self.pages(), 'run 1')

    def test_another_package_or_option_reads_them_again(self):
        self.assertEqual(self.pages(), 'run 1')
        self.upd.write_bytes(b'u' * 21)                       # another update file
        self.assertEqual(self.pages(), 'run 2')
        self.assertEqual(self.pages(upd=False), 'run 3')      # the base game alone
        with mock.patch.object(nsp, 'ALLOW_DAMAGED', True): self.assertEqual(self.pages(upd=False), 'run 4')
        self.assertEqual(self.pages(upd=False), 'run 5')      # and back

    def test_a_good_copy_of_the_same_size_reads_them_again(self):
        self.assertEqual(self.pages(), 'run 1')
        st = self.upd.stat(); os.utime(self.upd, ns=(st.st_atime_ns, st.st_mtime_ns + 10**9))   # the same name and size, copied in later
        self.assertEqual(self.pages(), 'run 2')

    def test_a_failed_extraction_leaves_no_key(self):
        self.assertEqual(self.pages(), 'run 1')
        self.change_nx_extract('// another\n')
        def fail(cmd, log, **kw): raise nsp.BuildError('nx_extract exit code 4')
        with mock.patch.object(nsp, 'run', fail), self.assertRaises(nsp.BuildError): self.pages()
        self.assertFalse((self.dst / 'html.key').exists())
        self.assertEqual(self.pages(), 'run 2')               # the old pages are not taken for this nx_extract's


class Program(Inputs):
    def test_the_same_inputs_reuse_the_image(self):
        self.assertEqual(self.program(), 'run 1')
        self.assertEqual(self.program(), 'run 1')
        self.assertEqual(self.runs[0][3:6], [str(self.upd), str(self.ex / 'v65536.partial'), '--romfs-image'])

    def test_a_changed_nx_extract_extracts_again_and_drops_the_pages(self):
        self.assertEqual(self.program(), 'run 1'); self.assertEqual(self.pages(), 'run 2')
        self.change_nx_extract('// reads a cartridge dump\'s base NCAs only\n')
        self.assertEqual(self.program(), 'run 3')
        self.assertFalse((self.dst / 'html').exists())        # the old folder went whole: the pages are read with this nx_extract too
        self.assertEqual(self.pages(), 'run 4')
        self.assertEqual(self.program(), 'run 3'); self.assertEqual(self.pages(), 'run 4')

    def test_an_image_from_a_build_before_the_key_is_extracted_again_once(self):
        (self.dst / 'ExeFS').mkdir(parents=True); (self.dst / 'ExeFS/main').write_text('old')
        rom = self.tmp / 'old'; (rom / 'CPK').mkdir(parents=True); (rom / 'CPK/ALL_ASIA.CPK').write_text('an older nx_extract'); romfs_image.pack(rom, self.dst / 'RomFS.img')
        self.assertEqual(self.program(), 'run 1')
        self.assertEqual(self.program(), 'run 1')

    def test_an_invalid_image_under_its_key_is_extracted_again(self):
        self.assertEqual(self.program(), 'run 1')
        img = self.dst / 'RomFS.img'; img.write_bytes(img.read_bytes()[:-1])   # its index cut short
        self.assertEqual(self.program(), 'run 2')
        self.assertEqual(self.program(), 'run 2')

    def test_a_damaged_image_is_not_kept_for_a_good_copy_or_without_the_option(self):
        with mock.patch.object(nsp, 'ALLOW_DAMAGED', True): self.assertEqual(self.program(), 'run 1')
        self.assertEqual(self.program(), 'run 2')             # packed again without --allow-damaged
        st = self.upd.stat(); os.utime(self.upd, ns=(st.st_atime_ns, st.st_mtime_ns + 10**9))
        self.assertEqual(self.program(), 'run 3')             # a good copy of the same name and size

    def test_a_nup_or_dump_folder_is_not_taken_for_an_extraction(self):
        self.assertEqual(self.program(), 'run 1')
        (self.dst / 'extract.key').unlink()                    # what extract_nup and extract_dump do before they write the folder
        self.assertEqual(self.program(), 'run 2')

    def test_a_failed_extraction_keeps_the_old_folder_and_no_key_for_it(self):
        self.assertEqual(self.program(), 'run 1')
        self.change_nx_extract('// another\n')
        def fail(cmd, log, **kw): raise nsp.BuildError('nx_extract exit code 4')
        with mock.patch.object(nsp, 'run', fail), self.assertRaises(nsp.BuildError): self.program()
        self.assertEqual((nsp.rfs.source(self.dst) / 'CPK/ALL_ASIA.CPK').read_text(), 'run 1')
        self.assertEqual(self.program(), 'run 2')


if __name__ == '__main__':
    unittest.main()
