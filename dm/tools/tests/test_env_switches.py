"""docs/env_switches.tsv lists every DM_* switch the code reads, each with a class and a meaning (dm/tools/env_switches.py writes it; class and meaning are kept by hand).
A switch the code reads that the table does not list, a row for a switch nothing reads any more, or a row without a meaning fails here.

`--verify <file>...` checks a text instead: every DM_* it names must be a real switch. It exists because two replies wrote DM_LOG_UEMALLOC and DM_NVN_ALIAS_TRACE from memory in
one day, and neither is a switch — a reader who trusts them wastes a run and learns nothing."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]


class EnvSwitches(unittest.TestCase):
    def test_table_is_current_and_complete(self):
        r = subprocess.run([sys.executable, str(REPO / 'dm/tools/env_switches.py'), '--check'], cwd=REPO, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_verify_accepts_a_real_switch_and_rejects_an_invented_one(self):
        with tempfile.TemporaryDirectory() as d:
            good = Path(d, 'good.md'); good.write_text('Set `DM_IMPORTS=1` to see the imports.\n')
            r = subprocess.run([sys.executable, str(REPO / 'dm/tools/env_switches.py'), '--verify', str(good)], cwd=REPO, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

            bad = Path(d, 'bad.md'); bad.write_text('Set `DM_LOG_UEMALLOC=1` to see the allocations.\n')
            r = subprocess.run([sys.executable, str(REPO / 'dm/tools/env_switches.py'), '--verify', str(bad)], cwd=REPO, capture_output=True, text=True)
            self.assertEqual(r.returncode, 1, 'an invented switch must fail the check')
            self.assertIn('DM_LOG_UEMALLOC', r.stdout)

    def test_verify_ignores_a_prefix_and_a_family(self):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d, 'prefixes.md'); f.write_text('`DM_PROF_<key>` overrides; `DM_NVN_*` are the graphics ones; `DM_MEMSTAT` is real.\n')
            r = subprocess.run([sys.executable, str(REPO / 'dm/tools/env_switches.py'), '--verify', str(f)], cwd=REPO, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == '__main__':
    unittest.main()
