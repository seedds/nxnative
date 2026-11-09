"""What flake.nix and dm/nix/shell_hook.sh read from other files still matches their patterns (CI has no Nix to evaluate the flake)."""
from pathlib import Path
import re
import shutil
import subprocess
import unittest

REPO = Path(__file__).resolve().parents[3]


class Flake(unittest.TestCase):
    def setUp(self):
        self.flake = (REPO / 'flake.nix').read_text()

    def test_lint_c_has_the_lines_the_flake_reads(self):
        lines = (REPO / '.github/workflows/lint-c.yml').read_text().split('\n')
        for key, value in (('LLVM_VERSION', r'\d+\.\d+\.\d+'), ('LLVM_SHA256', r'[0-9a-f]{64}')):
            self.assertIn(f'workflowEnv lintC "{key}"', self.flake)
            found = [m[1] for line in lines if (m := re.fullmatch(f' *{key}: *([^ #]+).*', line))]
            self.assertTrue(found, f'lint-c.yml has no {key} line for flake.nix')
            self.assertRegex(found[0], f'^{value}$', f'lint-c.yml: {key} as flake.nix reads it')

    def test_requirement_files_exist(self):
        files = [f for group in re.findall(r'requirements = \[([^\]]*)\]', self.flake) for f in re.findall(r'"([^"]+)"', group)]
        self.assertTrue(files, 'flake.nix names no requirement files')
        for f in files: self.assertTrue((REPO / '.github/requirements' / f).is_file(), f'flake.nix names .github/requirements/{f}, which does not exist')

    def test_fuzz_shell_installs_the_documented_atheris(self):
        self.assertRegex((REPO / '.github/requirements/fuzz.txt').read_text(), r'(?m)^atheris==')
        self.assertIn('NXNATIVE_PIP_SKIP = "atheris"', self.flake)
        commit = re.search(r'git\+https://github\.com/google/atheris@([0-9a-f]{40})', self.flake)
        if commit is None: self.fail('flake.nix names no atheris commit')
        self.assertIn(f'github.com/google/atheris@{commit[1]}', (REPO / 'docs/development.md').read_text(), 'flake.nix and docs/development.md name other atheris commits')

    def test_hook_reads_the_xcode_version(self):
        self.assertRegex((REPO / 'dm/gfx/tools/own_compilers.py').read_text(), r"(?m)^XCODE_VERSION = '[0-9.]+'")

    def test_hook_parses(self):
        bash = shutil.which('bash')
        if bash is None: self.skipTest('no bash')
        r = subprocess.run([bash, '-n', str(REPO / 'dm/nix/shell_hook.sh')], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == '__main__':
    unittest.main()
