"""The capstone versions the rewriter takes (dm/tools/capstone_version.py; docs/reference/rewriter.md, "The capstone version"): patch_nso.py and the NSP script's prerequisite
checks refuse any other with a [RED] line naming the version found and the one needed. Stand-in capstone packages (a module with its __version__ and, as pip and Ubuntu install
it, the distribution's metadata) in a temporary folder on PYTHONPATH, so it runs with or without a real capstone; no game files."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest

TOOLS = Path(__file__).resolve().parents[1]
REPO = TOOLS.parents[1]


class CapstoneVersion(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)

    def fake(self, name, module, dist=None, where=None):
        """A folder holding a stand-in capstone whose __version__ is module, with a dist-info of version dist beside it (in where instead: another folder's metadata)"""
        root = Path(self.tmp.name) / name
        (root / 'capstone').mkdir(parents=True)
        (root / 'capstone/__init__.py').write_text(f'__version__ = {module!r}\n')
        if dist:
            d = (where or root) / f'capstone-{dist}.dist-info'; d.mkdir(parents=True)
            (d / 'METADATA').write_text(f'Metadata-Version: 2.1\nName: capstone\nVersion: {dist}\n')
        return root

    def py(self, path, code, *args):   # code: a program's text, or the path of a script
        env = dict(os.environ, PYTHONPATH=os.pathsep.join([str(path), str(TOOLS)]), PYTHONNOUSERSITE='1')
        run = [str(code)] if isinstance(code, Path) else ['-c', code]
        return subprocess.run([sys.executable, *run, *args], env=env, capture_output=True, text=True, cwd=self.tmp.name)

    def problem(self, path):
        r = self.py(path, 'import capstone_version as c; print(c.installed()); print(c.problem())')
        self.assertEqual(r.returncode, 0, r.stderr)
        version, problem = (r.stdout.splitlines() + ['', ''])[:2]
        return version, problem

    def test_versions(self):
        for name, module, dist, version, ok in (
                ('pip-5.0.9', '5.0.7', '5.0.9', '5.0.9', True),       # 5.0.8 and 5.0.9 say 5.0.7 in __version__: the distribution's metadata is what counts
                ('pip-5.0.2', '5.0.2', '5.0.2', '5.0.2', True),
                ('no-metadata', '5.0.7', None, '5.0.7', True),        # no metadata: the module's own string
                ('ubuntu-4.0.2', '4.0.2', '4.0.2', '4.0.2', False),   # Ubuntu 24.04's python3-capstone: no udf
                ('pip-5.0.1', '5.0.1', '5.0.1', '5.0.1', False),      # drops operands
                ('pip-5.0.0', '5.0.0', '5.0.0.post1', '5.0.0.post1', False),   # rewrites as 5.0.9 does, but below the first of the unbroken run 5.0.2 to 5.0.9
                ('pip-6', '6.0.0', '6.0.0a11', '6.0.0a11', False)):   # no CS_ARCH_ARM64
            with self.subTest(name=name):
                got, problem = self.problem(self.fake(name, module, dist))
                self.assertEqual(got, version)
                if ok: self.assertEqual(problem, '')
                else: self.assertRegex(problem, rf'^capstone {version} \(.*\): the rewriter needs capstone 5\.0\.2 or later 5\.0\.x')

    def test_metadata_of_another_copy_is_not_taken(self):
        # a 5.0.9 dist-info in another folder (say, a system copy shadowed by this one) does not vouch for the 4.0.2 module imported first
        other = Path(self.tmp.name) / 'other'
        path = self.fake('shadow', '4.0.2', '5.0.9', where=other)
        r = self.py(os.pathsep.join([str(path), str(other)]), 'import capstone_version as c; print(c.installed())')
        self.assertEqual(r.stdout.strip(), '4.0.2', r.stderr)

    def test_rewriter_refuses(self):
        # patch_nso.py stops at its top, before it reads the NSO (which does not exist here) or imports lz4
        r = self.py(self.fake('ubuntu', '4.0.2', '4.0.2'), TOOLS / 'patch_nso.py', 'missing.nso', 'out', 'main')
        self.assertNotEqual(r.returncode, 0)
        self.assertRegex(r.stderr, r'^\[RED\] capstone 4\.0\.2 \(.*\): the rewriter needs capstone 5\.0\.2 or later 5\.0\.x')
        self.assertNotIn('Traceback', r.stderr)

    def test_packer_refuses_at_its_start(self):
        # build_ipa_from_nsp.py's prerequisite checks, the port's (--platform linux / windows) and the Apple one, before anything is extracted
        code = textwrap.dedent('''
            import importlib.util, sys
            from pathlib import Path
            spec = importlib.util.spec_from_file_location('nsp', sys.argv[1]); nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
            nsp._importable = lambda m: True   # lz4 and the others: there
            for check in (lambda: nsp.check_prereqs_port({'PATH': ''}, Path(sys.argv[2])), lambda: nsp.check_prereqs({'PATH': ''}, Path('/nonexistent'))):
                try: check(); print('passed')
                except nsp.BuildError as e: print(e)
        ''')
        for name, module, dist, refused in (('ubuntu', '4.0.2', '4.0.2', True), ('pip', '5.0.7', '5.0.9', False)):
            with self.subTest(name=name):
                r = self.py(self.fake(name, module, dist), code, str(REPO / 'scripts/build_ipa_from_nsp.py'), str(REPO))
                self.assertEqual(r.returncode, 0, r.stderr)
                lines = r.stdout.splitlines()
                self.assertEqual(len(lines), 2, r.stdout)
                for line in lines:
                    if refused: self.assertRegex(line, r'^capstone 4\.0\.2 \(.*\): the rewriter needs capstone 5\.0\.2 or later 5\.0\.x.*for the Python running this script')
                    else: self.assertNotIn('capstone', line)   # past the capstone check: dotnet, Xcode and the others are what this machine lacks


if __name__ == '__main__':
    unittest.main()
