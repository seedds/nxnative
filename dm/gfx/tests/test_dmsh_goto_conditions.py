#!/usr/bin/env python3
"""Ordinary CFG entry-condition definedness, paths, SPIR-V and Metal diagnostics."""
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


class GotoConditions(unittest.TestCase):
    @classmethod
    def run_checked(cls, command):
        result = subprocess.run(command, capture_output=True, text=True, timeout=90)
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
            raise unittest.SkipTest('needs .NET SDK10+, spirv-val and SPIRV-Cross')
        major = cls.run_checked([cls.dotnet, '--version']).strip().split('.')[0]
        if not major.isdigit() or int(major) < 10:
            raise unittest.SkipTest('needs .NET SDK10 or newer')
        cls.tmp = tempfile.TemporaryDirectory(prefix='dm-goto-conditions-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.work = Path(cls.tmp.name)
        command = [cls.dotnet, 'build', str(HERE / 'goto_conditions/Conditions.csproj'), '-c', 'Release',
                   '--disable-build-servers', '-p:UseSharedCompilation=false',
                   '--artifacts-path', str(cls.work / 'artifacts'), '-o', str(cls.work / 'bin')]
        if os.environ.get('DMSH_TEST_RYU_SRC'):
            command.append('-p:RyuSrc=' + str(Path(os.environ['DMSH_TEST_RYU_SRC']).resolve()))
        cls.run_checked(command)
        cls.validation = cls.run_checked([cls.dotnet, str(cls.work / 'bin/Conditions.dll'), str(cls.work)])
        for mode in ('optimized', 'debug'):
            source = cls.work / f'{mode}.spv'
            cls.run_checked([cls.validator, str(source)])
            cls.run_checked([cls.cross, str(source), '--msl', '--msl-version', '24000',
                             '--output', str(source.with_suffix('.metal'))])

    def test_exhaustive_cfg_paths_and_no_undefined_ast_reads(self):
        self.assertIn('3145728 path/output checks passed; no undefined AST reads', self.validation)

    @unittest.skipUnless(platform.system() == 'Darwin' and shutil.which('xcrun'), 'needs the macOS Metal compiler')
    def test_metal_has_no_conditional_uninitialized_reads(self):
        for mode in ('optimized', 'debug'):
            self.run_checked(['xcrun', '-sdk', 'macosx', 'metal', '-std=macos-metal2.4',
                              '-Werror=sometimes-uninitialized', '-Wno-unused-variable', '-c',
                              str(self.work / f'{mode}.metal'), '-o', str(self.work / f'{mode}.air')])


if __name__ == '__main__':
    unittest.main()
