"""build_nx_extract builds the tool once for its sources (issue #1181): a folder whose build_src.sha256 matches keeps its build, a folder built earlier in the run is
copied into the next one, and a change to the sources or to dotnet builds again. dotnet is not run: the build command is mocked to write the executable."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
if spec is None or spec.loader is None: raise ImportError('scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)


class NxExtractOnce(unittest.TestCase):
    def setUp(self):
        self.builds = []
        self.tmp = tempfile.TemporaryDirectory(); root = Path(self.tmp.name)
        self.repo = root / 'repo'; src = self.repo / 'dm/tools/nx_extract'; src.mkdir(parents=True)
        (src / 'Program.cs').write_text('class P {}\n'); (src / 'nx_extract.csproj').write_text('<Project/>\n')
        self.logs = root / 'logs'; self.logs.mkdir()
        self.dotnet = '10.0.403'
        self.patches = [mock.patch.object(nsp, 'run', self.fake_run), mock.patch.dict(nsp.NX_EXTRACT_BUILT, clear=True),
                        mock.patch.object(nsp.subprocess, 'run', self.fake_version)]
        for p in self.patches: p.start()

    def tearDown(self):
        for p in self.patches: p.stop()
        self.tmp.cleanup()

    def fake_run(self, cmd, log, env=None, **kw):   # dotnet build -o <out>: writes the executable and the library there
        self.assertEqual(cmd[:2], ['dotnet', 'build']); out = Path(cmd[cmd.index('-o') + 1])
        (out / 'nx_extract').write_text('exe'); (out / 'LibHac.dll').write_text('lib'); self.builds.append(out)

    def fake_version(self, cmd, **kw):   # dotnet --version, for the key
        return mock.Mock(stdout=self.dotnet + '\n', stderr='')

    def build(self, name):
        work = Path(self.tmp.name) / name
        return nsp.build_nx_extract(self.repo, work, self.logs, {'PATH': ''})

    def test_once_per_sources(self):
        nx = self.build('a_build')
        self.assertTrue(nx.exists() and (nx.parent / 'LibHac.dll').exists() and (nx.parent / 'build_src.sha256').exists())
        self.assertEqual(len(self.builds), 1)
        self.assertEqual(self.build('a_build'), nx); self.assertEqual(len(self.builds), 1, 'the same folder keeps its build')
        other = self.build('a_survey')
        self.assertEqual(len(self.builds), 1, 'a second folder in the same run is a copy of the first')
        self.assertTrue(other.exists() and (other.parent / 'LibHac.dll').exists() and (other.parent / 'build_src.sha256').read_text() == (nx.parent / 'build_src.sha256').read_text())
        # Another run (nothing remembered): the stamped folders still keep their build
        nsp.NX_EXTRACT_BUILT.clear()
        self.assertEqual(self.build('a_build'), nx); self.assertEqual(self.build('a_survey'), other); self.assertEqual(len(self.builds), 1)
        # A folder whose build went away is built again, and the copy of it in this run is not reused once it is gone
        nsp.rmtree(nx.parent); self.build('a_build'); self.assertEqual(len(self.builds), 2)

    def test_changed_sources_or_dotnet_build_again(self):
        self.build('a_build'); self.assertEqual(len(self.builds), 1)
        (self.repo / 'dm/tools/nx_extract/Program.cs').write_text('class P { static int x; }\n')
        self.build('a_build'); self.assertEqual(len(self.builds), 2, 'changed sources build again')
        self.build('a_survey'); self.assertEqual(len(self.builds), 2, 'and the new build is what the next folder copies')
        self.dotnet = '10.0.500'
        self.build('a_build'); self.assertEqual(len(self.builds), 3, 'another dotnet builds again')

    def test_a_build_without_the_executable_fails(self):
        with mock.patch.object(nsp, 'run', lambda cmd, log, **k: None), self.assertRaises(nsp.BuildError): self.build('a_build')
        self.assertFalse((Path(self.tmp.name) / 'a_build/tools/nx_extract/build_src.sha256').exists())


if __name__ == '__main__': unittest.main()
