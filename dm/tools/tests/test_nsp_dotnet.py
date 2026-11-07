"""NSP script: Homebrew's dotnet wrapper must supply its runtime root to standalone apphosts.
No game files, keys or .NET installation needed."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)


class Dotnet(unittest.TestCase):
    def setUp(self):
        cm = mock.patch.dict(os.environ, {'PATH': '/tools/bin'}, clear=True)
        cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def test_homebrew_runtime_reaches_explicit_and_inherited_children(self):
        root = '/external/Tools With Spaces/Cellar/dotnet/10.0.402/libexec'
        env = dict(os.environ, DOTNET_ROOT_X64='/caller/x64')
        out = f'Microsoft.AspNetCore.App 10.0.13 [{root}/shared/Microsoft.AspNetCore.App]\n' + \
              f'Microsoft.NETCore.App 10.0.13 [{root}/shared/Microsoft.NETCore.App]\n'
        with mock.patch.object(nsp.shutil, 'which', return_value='/tools/bin/dotnet') as which, \
             mock.patch.object(nsp.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, out)) as run:
            nsp.configure_dotnet(env)
            which.assert_called_once_with('dotnet', path='/tools/bin')
            self.assertIs(run.call_args.kwargs['env'], env)
        cmd = [sys.executable, '-c', 'import json, os; print(json.dumps(os.environ.get("DOTNET_ROOT")))']
        for child_env in (env, None):
            r = subprocess.run(cmd, env=child_env, capture_output=True, text=True, check=True)
            self.assertEqual(json.loads(r.stdout), root)
        self.assertEqual(env['DOTNET_ROOT_X64'], '/caller/x64')

    def test_caller_root_wins(self):
        os.environ.update(DOTNET_ROOT='/caller/runtime', DOTNET_ROOT_ARM64='/caller/arm64')
        env = dict(os.environ)
        with mock.patch.object(nsp.subprocess, 'run') as run:
            nsp.configure_dotnet(env)
            run.assert_not_called()
        self.assertEqual(env, dict(os.environ))
        self.assertEqual(env['DOTNET_ROOT'], '/caller/runtime')
        self.assertEqual(env['DOTNET_ROOT_ARM64'], '/caller/arm64')

    def test_missing_dotnet_leaves_environment_untouched(self):
        env = dict(os.environ)
        with mock.patch.object(nsp.shutil, 'which', return_value=None), mock.patch.object(nsp.subprocess, 'run') as run:
            nsp.configure_dotnet(env)
            run.assert_not_called()
        self.assertEqual(env, dict(os.environ))

    def test_failed_or_malformed_probe_does_not_export_wrapper_directory(self):
        outputs = ('', 'Microsoft.AspNetCore.App 10.0.13 [/runtime/shared/Microsoft.AspNetCore.App]',
                   'Microsoft.NETCore.App 10.0.13 [/runtime/shared/Microsoft.NETCore.App',
                   'Microsoft.NETCore.App 10.0.13 [/not/a/runtime]')
        failures = (OSError('cannot execute dotnet'), subprocess.TimeoutExpired('dotnet', 10), subprocess.CalledProcessError(1, 'dotnet'))
        for result in (*outputs, *failures):
            with self.subTest(result=result):
                env = dict(os.environ)
                kw = {'side_effect': result} if isinstance(result, Exception) else {'return_value': subprocess.CompletedProcess([], 0, result)}
                with mock.patch.object(nsp.shutil, 'which', return_value='/homebrew/bin/dotnet'), mock.patch.object(nsp.subprocess, 'run', **kw):
                    nsp.configure_dotnet(env)
                self.assertNotIn('DOTNET_ROOT', env)
                self.assertNotIn('DOTNET_ROOT', os.environ)


class Sdk10First(unittest.TestCase):
    """An older .NET earlier on PATH (an installer's /usr/local/share/dotnet before ~/.dotnet) gives way to the first dotnet with an SDK 10."""
    def setUp(self):
        t = tempfile.TemporaryDirectory(); self.addCleanup(t.cleanup); self.tmp = Path(t.name)
        self.tools = str(self.tmp / 'tools'); os.mkdir(self.tools)   # a PATH entry without dotnet

    def dotnet(self, name, sdks, code=0):
        d = self.tmp / name; d.mkdir()
        (d / 'dotnet').write_text(f'#!/bin/sh\nprintf "{sdks}"\nexit {code}\n'); (d / 'dotnet').chmod(0o755)
        return str(d)

    def path_after(self, *dirs):
        env = {'PATH': os.pathsep.join(dirs)}
        with mock.patch.object(nsp, 'say') as say: nsp.dotnet_sdk10_first(env)
        self.said = say.call_args
        return env['PATH'].split(os.pathsep)

    def test_older_sdk_in_front_gives_way(self):
        old, home = self.dotnet('share', '7.0.402 [/s/sdk]\\n'), self.dotnet('home', '9.0.100 [/h/sdk]\\n10.0.403 [/h/sdk]\\n')
        self.assertEqual(self.path_after(self.tools, old, home, '/usr/bin'), [home, self.tools, old, '/usr/bin'])
        self.assertIn(old, self.said.args[0])

    def test_preview_counts(self):
        old, new = self.dotnet('share', '8.0.318 [/s/sdk]\\n'), self.dotnet('home', '11.0.100-preview.1 [/h/sdk]\\n')
        self.assertEqual(self.path_after(old, new), [new, old])

    def test_first_with_sdk10_keeps_the_order(self):
        local, home = self.dotnet('local', '10.0.403 [/l/sdk]\\n'), self.dotnet('home', '10.0.100 [/h/sdk]\\n')
        dirs = [self.tools, local, home]
        self.assertEqual(self.path_after(*dirs), dirs)
        self.assertIsNone(self.said)

    def test_no_sdk10_keeps_the_order(self):
        dirs = [self.dotnet('share', '7.0.402 [/s/sdk]\\n'), self.dotnet('home', '8.0.318 [/h/sdk]\\n9.0.100 [/h/sdk]\\n')]
        self.assertEqual(self.path_after(*dirs), dirs)

    def test_failing_dotnet_is_skipped(self):
        broken, home = self.dotnet('broken', '', code=1), self.dotnet('home', '10.0.403 [/h/sdk]\\n')
        self.assertEqual(self.path_after(broken, home, broken), [home, broken, broken])


if __name__ == '__main__': unittest.main()
