"""NSP script: a registered version outside the profile's shader_versions keeps the profile's tables only when their map holds every shader its own split finds (uncovered_shaders);
otherwise the build takes a shader-only derived profile. No game files needed (temporary files only in a temporary folder)."""
import importlib.util
from pathlib import Path
import shutil
import tempfile
import types
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
SCRIPT = REPO / 'scripts/build_ipa_from_nsp.py'
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', SCRIPT)
assert spec is not None and spec.loader is not None
nsp =importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
HEAD = 'package\tindex\tstage\tdm_ctl_sha16\tsource\n'


def rows(*keys):
    return HEAD + ''.join(f'ps.bin\t{i}\tpixel\t{k}\t/Shader/a.bsh#{i}\n' for i, k in enumerate(keys))


class Cover(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        (self.tmp / 'profile').mkdir(); (self.tmp / 'logs').mkdir()
        (self.tmp / 'profile/shader_map.tsv').write_text(rows('1111111111111111', '2222222222222222', '3333333333333333'))
        self.g = types.SimpleNamespace(table=lambda n: self.tmp / 'profile' / n)

    def packed(self, *keys):   # the packed version's split writes this map (the first table argument after the package path)
        def fake_run(cmd, log, env=None, **kw):
            Path(cmd[5]).write_text(rows(*keys))
        with mock.patch.object(nsp, 'run', fake_run):
            return nsp.uncovered_shaders('python3', REPO, self.tmp / 'v1', self.tmp / 'shader/pkg', {}, self.tmp / 'logs', self.g)

    def test_every_shader_in_the_map_keeps_the_profile(self):
        self.assertEqual(self.packed('1111111111111111', '3333333333333333'), set())

    def test_a_shader_not_in_the_map_derives(self):
        self.assertEqual(self.packed('1111111111111111', '4444444444444444'), {'4444444444444444'})

    def test_the_split_output_is_removed(self):
        self.packed('1111111111111111')
        self.assertFalse((self.tmp / 'shader/pkg.cover').exists())


if __name__ == '__main__':
    unittest.main()
