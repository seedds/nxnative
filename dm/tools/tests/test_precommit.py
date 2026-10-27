"""The commit hook (.pre-commit-config.yaml) lints with the packages CI pins (.github/requirements/lint.txt): with other versions, its ruff and pyright findings would differ from
the lint job's, and the hook would pass or stop commits that CI judges the other way. Each hook that runs a test names one that exists. No pre-commit or network needed."""
from pathlib import Path
import re
import unittest

REPO = Path(__file__).resolve().parents[3]
CONFIG = (REPO / '.pre-commit-config.yaml').read_text()


class Hook(unittest.TestCase):
    def test_lint_packages_are_the_ci_pins(self):
        pins = sorted(line.split('#')[0].strip() for line in (REPO / '.github/requirements/lint.txt').read_text().splitlines() if line.split('#')[0].strip())
        m = re.search(r'id: lint-python\n(?:\s+\w+:.*\n)*?\s+additional_dependencies: \[(.*)\]', CONFIG)
        if m is None: self.fail('.pre-commit-config.yaml: no additional_dependencies for the lint-python hook')
        hook = sorted(p.strip() for p in m.group(1).split(','))
        self.assertEqual(hook, pins, '.pre-commit-config.yaml: the lint-python hook\'s packages must be .github/requirements/lint.txt\'s pins')

    def test_hook_tests_exist(self):
        for target in re.findall(r'entry: python3 -m unittest (\S+)', CONFIG):
            path = target if target.endswith('.py') else '/'.join(target.split('.')[:4]) + '.py'
            self.assertTrue((REPO / path).is_file(), f'.pre-commit-config.yaml runs {target}, but {path} does not exist')


if __name__ == '__main__':
    unittest.main()
