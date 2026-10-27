"""NSP script early tool checks (scripts/build_ipa_from_nsp.py): the shader translator must start with the .NET it was given, and an OpenGL game needs glslangValidator,
both reported before the captures and the shader split instead of as an empty `xlate_source: lock ... / actual` or a late build_gl_shaders error.
No .NET, glslang, game files or Xcode needed (subprocess and PATH are mocked)."""
import importlib.util
from pathlib import Path
import subprocess
import unittest
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[3] / 'scripts/build_ipa_from_nsp.py'
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', SCRIPT)
assert spec and spec.loader
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
XL = Path('/repo/dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate')


def done(rc, out='', err=''): return subprocess.CompletedProcess([], rc, out, err)


class Translator(unittest.TestCase):
    def check(self, **kw):
        with mock.patch.object(nsp.subprocess, 'run', **kw) as run:
            nsp.check_translator(XL, {'PATH': '/bin', 'DOTNET_ROOT': '/stale'})
        return run

    def test_missing_runtime_stops_with_the_apphosts_first_line(self):
        with self.assertRaises(nsp.BuildError) as e:
            self.check(return_value=done(131, '', 'You must install .NET to run this application.\n\nApp: x'))
        self.assertIn('does not run with the .NET it was given (You must install .NET to run this application.)', str(e.exception))
        self.assertIn('DOTNET_ROOT', str(e.exception))

    def test_empty_answer_stops(self):   # exit 0 without a hash is what dmsh_chain.py turned into an empty `xlate_source`
        with self.assertRaises(nsp.BuildError): self.check(return_value=done(0, '  \n'))

    def test_unstartable_or_hung_translator_stops(self):
        for exc in (OSError('Exec format error'), subprocess.TimeoutExpired('x', 120)):
            with self.subTest(exc=exc), self.assertRaises(nsp.BuildError) as e: self.check(side_effect=exc)
            self.assertIn('does not start', str(e.exception))

    def test_working_translator_passes_with_the_scripts_environment_and_a_timeout(self):
        run = self.check(return_value=done(0, 'abc123\n'))
        self.assertEqual(run.call_args.args[0], [str(XL), '--source-sha'])
        self.assertEqual(run.call_args.kwargs['env']['DOTNET_ROOT'], '/stale')
        self.assertTrue(run.call_args.kwargs['timeout'])


class Glslang(unittest.TestCase):
    def test_missing_stops_and_present_passes(self):
        with mock.patch.object(nsp.shutil, 'which', return_value=None) as which:
            with self.assertRaises(nsp.BuildError) as e: nsp.check_glslang({'PATH': '/bin'})
            which.assert_called_once_with('glslangValidator', path='/bin')
        self.assertIn('brew install glslang', str(e.exception))
        with mock.patch.object(nsp.shutil, 'which', return_value='/bin/glslangValidator'): nsp.check_glslang({'PATH': '/bin'})

    def test_gl_shader_step_uses_the_same_check(self):
        with mock.patch.object(nsp, 'PROG', mock.Mock()), mock.patch.object(nsp.shutil, 'which', return_value=None), self.assertRaises(nsp.BuildError):
            nsp.build_gl_shaders('py', Path('/repo'), [], Path('/sh'), {'PATH': '/bin'}, Path('/logs'), mock.Mock())


class Order(unittest.TestCase):
    """main() runs the checks by graphics API and before the first capture: the translator and Metal for NVN only, glslang for OpenGL only"""
    def test_main_calls_them_before_the_captures(self):
        t = SCRIPT.read_text()
        nvn, gl = t.index("if gfx == 'nvn': check_metal(py, repo, env); check_translator(xlate, env)"), t.index("if gfx == 'gl': check_glslang(env)")
        self.assertLess(max(nvn, gl), t.index('PROG.add(glslc + vkcap'))
        self.assertLess(t.index("gfx = src['graphics'][0]"), min(nvn, gl))


if __name__ == '__main__': unittest.main()
