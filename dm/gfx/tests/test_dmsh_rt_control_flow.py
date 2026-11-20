#!/usr/bin/env python3
"""Synthetic production decoder/RPC validation; no captured game data or GPU required."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


class RayTracingControlFlow(unittest.TestCase):
    def test_native_driver_transfers_and_rejections(self):
        dotnet = shutil.which(os.environ.get('DMSH_TEST_DOTNET', 'dotnet'))
        if not dotnet:
            self.skipTest('needs .NET SDK10 or newer')
        version = subprocess.run([dotnet, '--version'], capture_output=True, text=True)
        major = version.stdout.strip().split('.')[0]
        if version.returncode or not major.isdigit() or int(major) < 10:
            self.skipTest('needs .NET SDK10 or newer')
        with tempfile.TemporaryDirectory(prefix='dm-rt-control-flow-') as tmp:
            work = Path(tmp)
            command = [dotnet, 'build', str(HERE / 'rt_control_flow/ControlFlow.csproj'), '-c', 'Release',
                       '--artifacts-path', str(work / 'artifacts'), '-o', str(work / 'bin')]
            if os.environ.get('DMSH_TEST_RYU_SRC'):
                command.append('-p:RyuSrc=' + str(Path(os.environ['DMSH_TEST_RYU_SRC']).resolve()))
            built = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([dotnet, str(work / 'bin/ControlFlow.dll')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('checks passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
