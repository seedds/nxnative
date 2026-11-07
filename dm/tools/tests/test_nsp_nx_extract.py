"""NSP script: nx_extract is built once per run however many folders need it, and a copy whose sources and SDK are unchanged is used as it is (#1181).
No game files, keys or .NET installation needed."""
import importlib.util
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
assert spec and spec.loader
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)


class BuildOnce(unittest.TestCase):
    def setUp(self):
        t = tempfile.TemporaryDirectory(); self.addCleanup(t.cleanup)
        self.root = Path(t.name)
        self.repo = self.root / 'repo'; src = self.repo / 'dm/tools/nx_extract'; src.mkdir(parents=True)
        (src / 'Program.cs').write_text('class P {}\n'); (src / 'nx_extract.csproj').write_text('<Project />\n')
        self.builds = []
        def fake_run(cmd, log, **kw):   # `dotnet build ... -o <out>` makes the executable and a library
            self.builds.append(cmd)
            out = Path(cmd[cmd.index('-o') + 1]); (out / 'nx_extract').write_text('exe'); (out / 'LibHac.dll').write_text('dll')
        self.sdk = '10.0.100'
        for cm in (mock.patch.object(nsp, '_NX_BUILT', {}), mock.patch.object(nsp, 'run', fake_run),
                   mock.patch.object(nsp, 'fingerprint', self.fingerprint)):
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def fingerprint(self, *items, env=None, real=nsp.fingerprint):   # `dotnet --version` answers self.sdk
        return real(*(('sdk', self.sdk) if isinstance(i, tuple) and i[0] == 'cmd' else i for i in items), env=env)

    def build(self, name):
        work = self.root / name
        nx = nsp.build_nx_extract(self.repo, work, work / 'logs', {})
        self.assertEqual((nx.read_text(), (nx.parent / 'LibHac.dll').read_text()), ('exe', 'dll'))
        return nx

    def new_run(self):
        for p in nsp._NX_BUILT.values(): shutil.rmtree(p, ignore_errors=True)
        nsp._NX_BUILT.clear()

    def test_one_build_for_every_folder_of_a_run(self):
        for name in ('probe', 'survey', 'survey', 'build'): self.build(name)
        self.assertEqual(len(self.builds), 1)

    def test_next_run_uses_the_copy_as_it_is(self):
        self.build('build'); self.new_run()
        self.build('build')
        self.assertEqual(len(self.builds), 1)

    def test_changed_source_sdk_or_copy_builds_again(self):
        self.build('build')
        for change in (lambda: (self.repo / 'dm/tools/nx_extract/Program.cs').write_text('class Q {}\n'),
                       lambda: setattr(self, 'sdk', '10.0.200'),
                       lambda: (self.root / 'build/tools/nx_extract/LibHac.dll').write_text('damaged')):
            n = len(self.builds); self.new_run(); change()
            self.build('build')
            self.assertEqual(len(self.builds), n + 1)

    def test_failed_build_is_not_kept(self):
        with mock.patch.object(nsp, 'run', lambda cmd, log, **kw: None):
            with self.assertRaises(nsp.BuildError): self.build('build')
        self.assertEqual(nsp._NX_BUILT, {})
        self.build('build')
        self.assertEqual(len(self.builds), 1)


if __name__ == '__main__':
    unittest.main()
