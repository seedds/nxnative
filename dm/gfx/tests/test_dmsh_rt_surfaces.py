#!/usr/bin/env python3
"""Synthetic RT formatted-store dispatch, native decoding and typed SPIR-V.

No game assets or GPU required. Needs .NET SDK 10+, spirv-val and SPIRV-Cross.
"""
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


class RayTracingSurfaces(unittest.TestCase):
    @classmethod
    def run_checked(cls, command, timeout=90):
        result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
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
        version = cls.run_checked([cls.dotnet, '--version'])
        major = version.strip().split('.')[0]
        if not major.isdigit() or int(major) < 10:
            raise unittest.SkipTest('needs .NET SDK 10 or newer')
        cls.tmp = tempfile.TemporaryDirectory(prefix='dm-rt-surfaces-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.work = Path(cls.tmp.name)
        command = [cls.dotnet, 'build', str(HERE / 'rt_surfaces/RtSurfaces.csproj'), '-c', 'Release',
                   '--disable-build-servers', '-p:UseSharedCompilation=false',
                   '--artifacts-path', str(cls.work / 'artifacts'), '-o', str(cls.work / 'bin')]
        if os.environ.get('DMSH_TEST_RYU_SRC'):
            command.append('-p:RyuSrc=' + str(Path(os.environ['DMSH_TEST_RYU_SRC']).resolve()))
        cls.run_checked(command)
        cls.validation = cls.run_checked([cls.dotnet, str(cls.work / 'bin/RtSurfaces.dll'), str(cls.work)])

    def test_class_control_flow_native_surface_forms_and_slot_bounds(self):
        self.assertIn('checks passed', self.validation)

    def test_generated_spirv_has_distinct_metal_component_types(self):
        source = self.work / 'surfaces.spv'
        self.run_checked([self.validator, '--target-env', 'vulkan1.2', str(source)])
        metal = source.with_suffix('.metal')
        self.run_checked([self.cross, str(source), '--msl', '--msl-version', '40000', '--output', str(metal)])
        text = metal.read_text()
        for component in ('float', 'int', 'uint'):
            self.assertIn(f'texture2d<{component}, access::write>', text)
        self.assertEqual(text.count('.write('), 6)


if __name__ == '__main__':
    unittest.main()
