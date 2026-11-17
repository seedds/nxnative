#!/usr/bin/env python3
"""Synthetic native-function state and typed traversal through production code.

Needs .NET SDK 10+, spirv-val and SPIRV-Cross. DMSH_TEST_GPU=1 checks all state,
hit, miss, procedural-rejection and guard words on Metal. No game data required.
"""
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
FIXTURE = HERE / 'native_functions'


class NativeFunctions(unittest.TestCase):
    @classmethod
    def run_checked(cls, command, **kwargs):
        result = subprocess.run(command, capture_output=True, text=True, **kwargs)
        if result.returncode:
            raise AssertionError(f'{command!r}\n{result.stdout}\n{result.stderr}')
        return result.stdout

    @classmethod
    def setUpClass(cls):
        cls.dotnet = shutil.which(os.environ.get('DMSH_TEST_DOTNET', 'dotnet'))
        cls.validator = shutil.which('spirv-val')
        cls.cross = shutil.which('spirv-cross')
        bundled = HERE.parent / 'tools/spirv_cross_bin/spirv-cross'
        if not cls.cross and platform.system() == 'Darwin' and bundled.is_file():
            cls.cross = str(bundled)
        if not all((cls.dotnet, cls.validator, cls.cross)):
            raise unittest.SkipTest('needs .NET SDK 10+, spirv-val and SPIRV-Cross')
        version = subprocess.run([cls.dotnet, '--version'], capture_output=True, text=True)
        major = version.stdout.strip().split('.')[0]
        if version.returncode or not major.isdigit() or int(major) < 10:
            raise unittest.SkipTest('needs .NET SDK 10 or newer')
        cls.tmp = tempfile.TemporaryDirectory(prefix='dm-native-functions-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.work = Path(cls.tmp.name)
        command = [cls.dotnet, 'build', str(FIXTURE / 'NativeFunctions.csproj'), '-c', 'Release',
                   '--artifacts-path', str(cls.work / 'artifacts'), '-o', str(cls.work / 'bin')]
        if os.environ.get('DMSH_TEST_RYU_SRC'):
            command.append('-p:RyuSrc=' + str(Path(os.environ['DMSH_TEST_RYU_SRC']).resolve()))
        cls.run_checked(command)
        cls.validation = cls.run_checked([cls.dotnet, str(cls.work / 'bin/NativeFunctions.dll'), str(cls.work)])
        for kind in ('state', 'trace'):
            for mode in ('optimized', 'debug'):
                source = cls.work / f'{kind}-{mode}.spv'
                cls.run_checked([cls.validator, str(source)])
                cls.run_checked([cls.cross, str(source), '--msl', '--msl-version', '40000',
                                 '--msl-decoration-binding', '--output', str(source.with_suffix('.metal'))])

    def test_native_validation_and_spirv(self):
        self.assertIn('5 unsupported operations rejected', self.validation)
        self.assertIn('Native resource inputs: module AS slots and 6 unsupported forms checked', self.validation)

    @unittest.skipUnless(os.environ.get('DMSH_TEST_GPU') == '1', 'set DMSH_TEST_GPU=1 for Metal execution')
    def test_state_and_real_ray_query_on_gpu(self):
        if platform.system() != 'Darwin' or not shutil.which('clang'):
            self.skipTest('needs macOS, clang and a Metal device')
        executable = self.work / 'gpu'
        self.run_checked(['clang', '-O1', '-Wall', '-Wextra', '-Werror', '-fobjc-arc', str(FIXTURE / 'gpu.m'),
                          '-framework', 'Foundation', '-framework', 'Metal', '-o', str(executable)])
        result = subprocess.run([str(executable), str(self.work)], capture_output=True, text=True,
                                env=dict(os.environ, MTL_DEBUG_LAYER='1'))
        if result.returncode == 77:
            self.skipTest('needs Metal ray tracing')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for kind, words in (('state', 736), ('trace', 288)):
            for mode in ('optimized', 'debug'):
                self.assertIn(f'{kind}-{mode}: {words} words, 0 mismatches', result.stdout)


if __name__ == '__main__':
    unittest.main()
