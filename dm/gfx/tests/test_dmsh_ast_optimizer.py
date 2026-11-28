#!/usr/bin/env python3
"""Ordinary IR calls and memory snapshots through AST optimization and SPIR-V.

Needs .NET SDK 10+, spirv-val and SPIRV-Cross. DMSH_TEST_GPU=1 also checks every
data/guard word on Metal. Build artifacts live in a temporary directory. Optional
DMSH_TEST_DOTNET and DMSH_TEST_RYU_SRC select an SDK or candidate Shader source tree.
"""
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
FIXTURE = HERE / 'ast_optimizer'


class AstOptimizer(unittest.TestCase):
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
        cls.tmp = tempfile.TemporaryDirectory(prefix='dm-ast-optimizer-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.work = Path(cls.tmp.name)
        command = [cls.dotnet, 'build', str(FIXTURE / 'AstOptimizer.csproj'), '-c', 'Release',
                   '--artifacts-path', str(cls.work / 'artifacts'), '-o', str(cls.work / 'bin')]
        if os.environ.get('DMSH_TEST_RYU_SRC'):
            command.append('-p:RyuSrc=' + str(Path(os.environ['DMSH_TEST_RYU_SRC']).resolve()))
        cls.run_checked(command)
        cls.run_checked([cls.dotnet, str(cls.work / 'bin/AstOptimizer.dll'), str(cls.work)])

    @classmethod
    def run_checked(cls, command, **kwargs):
        result = subprocess.run(command, capture_output=True, text=True, **kwargs)
        if result.returncode:
            raise AssertionError(f'{command!r}\n{result.stdout}\n{result.stderr}')
        return result.stdout

    def cross_modes(self):
        for mode in ('optimized', 'debug'):
            source = self.work / f'{mode}.spv'
            self.run_checked([self.validator, str(source)])
            self.run_checked([self.cross, str(source), '--msl', '--msl-version', '24000',
                              '--msl-decoration-binding', '--output', str(source.with_suffix('.metal'))])

    def test_call_lvalues_and_effects_produce_valid_spirv(self):
        self.cross_modes()

    @unittest.skipUnless(os.environ.get('DMSH_TEST_GPU') == '1', 'set DMSH_TEST_GPU=1 for Metal execution')
    def test_call_results_and_memory_order_on_gpu(self):
        if platform.system() != 'Darwin' or not shutil.which('clang'):
            self.skipTest('needs macOS, clang and a Metal device')
        self.cross_modes()
        executable = self.work / 'gpu'
        self.run_checked(['clang', '-O1', '-Wall', '-Wextra', '-Werror', '-fobjc-arc', str(FIXTURE / 'gpu.m'),
                          '-framework', 'Foundation', '-framework', 'Metal', '-o', str(executable)])
        result = subprocess.run([str(executable), str(self.work)], capture_output=True, text=True,
                                env=dict(os.environ, MTL_DEBUG_LAYER='1'))
        if result.returncode == 77:
            self.skipTest('no Metal device')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('optimized: 64 words, 0 mismatches', result.stdout)
        self.assertIn('debug: 64 words, 0 mismatches', result.stdout)


if __name__ == '__main__':
    unittest.main()
