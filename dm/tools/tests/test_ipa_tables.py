"""The iOS packager's production table-copy step, with synthetic files and no game or SDK."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]


class IpaTables(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.profile, self.patched, self.tables = [self.root / n for n in ('profile', 'patched', 'tables')]
        for p in (self.profile, self.patched, self.tables): p.mkdir()
        for name in ('shader_map.tsv', 'c1_full.tsv'):
            (self.profile / name).write_text(name + '\n')
        for suffix in ('fixups', 'gates', 'cuts', 'obs'):
            (self.patched / f'main.{suffix}.tsv').write_text(suffix + '\n')

    def stage(self, api, setup=''):
        script = (REPO / 'dm/ios/tools/build_ipa.sh').read_text()
        start = script.index('program_tables() {')
        end = script.index('\n}', start) + 2
        env = dict(os.environ, GD=str(self.profile), PATCHED=str(self.patched), TD=str(self.tables),
                   GFXAPI=api, DM=str(REPO / 'dm'), GAME='test', VNACP='absent')
        # Settings generation is outside this test. All table copies use the real cp.
        helpers = 'set -u\ndie() { echo "FAIL: $*"; exit 1; }\nt() { echo "$*"; }\npython3() { :; }\n'
        return subprocess.run(['bash', '-c', helpers + setup + script[start:end] + '\nprogram_tables\n'],
                              env=env, capture_output=True, text=True)

    def test_no_recorded_seed_is_optional_for_every_api(self):
        for api in ('nvn', 'vulkan', 'gl'):
            with self.subTest(api=api):
                result = self.stage(api)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual((self.tables / 'shader_map.tsv').read_bytes(), b'shader_map.tsv\n')
                self.assertFalse((self.tables / 'pso_manifest.tsv').exists())
                self.assertIn('pso_manifest.tsv', result.stdout)

    def test_recorded_seed_is_preserved_for_every_api(self):
        seed = b'# recorded list\nS\t1234567890abcdef\tvertex\nC\tabcdef1234567890\n'
        (self.profile / 'pso_manifest.tsv').write_bytes(seed)
        for api in ('nvn', 'vulkan', 'gl'):
            with self.subTest(api=api):
                result = self.stage(api)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual((self.tables / 'pso_manifest.tsv').read_bytes(), seed)

    def test_missing_required_shader_map_still_fails(self):
        (self.profile / 'shader_map.tsv').unlink()
        for api in ('nvn', 'vulkan', 'gl'):
            with self.subTest(api=api):
                self.assertNotEqual(self.stage(api).returncode, 0)

    def test_missing_required_nvn_constants_still_fails(self):
        (self.profile / 'c1_full.tsv').unlink()
        self.assertNotEqual(self.stage('nvn').returncode, 0)

    def test_seed_copy_failure_still_fails(self):
        (self.profile / 'pso_manifest.tsv').write_text('C\t1234567890abcdef\n')
        # A present seed must not silently disappear when the destination refuses writes.
        result = self.stage('nvn', 'cp() { for arg in "$@"; do case "$arg" in */pso_manifest.tsv) return 1;; esac; done; command cp "$@"; }\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('FAIL:', result.stdout)


if __name__ == '__main__': unittest.main()
