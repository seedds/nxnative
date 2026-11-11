"""The iOS app's CPU target: the oldest chip the app supports, not the newest.

dav1d (dm/third_party/dav1d/src/cpu.h) adds every CPU feature the compiler's target promises to the features it detects at run time, so a target newer than
the device runs instructions the device lacks: with -mcpu=apple-a18, Ocarina of Time's title movie stopped an M1 iPad Pro (no I8MM) with SIGILL in
put_8tap_neon_i8mm. The oldest supported chip is the M1 (README: an iPad with an M1, an iPhone from the A17 Pro, as the games need BC textures); the host's
assembly needs LSE atomics (ARMv8.1), which it has."""
import re
import shutil
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
# What the M1 has (the oldest supported chip) and what it lacks (each promised by a newer target: I8MM and BF16 from the A15, SME from the M4 / A18).
NEEDED = ('__ARM_FEATURE_ATOMICS',)
ABSENT = ('__ARM_FEATURE_MATMUL_INT8', '__ARM_FEATURE_BF16_VECTOR_ARITHMETIC', '__ARM_FEATURE_SME')


def macros(mcpu):
    out = subprocess.run(['xcrun', '-sdk', 'iphoneos', 'clang', '-target', 'arm64-apple-ios26.0', f'-mcpu={mcpu}', '-dM', '-E', '-x', 'c', '/dev/null'],
                         capture_output=True, text=True, check=True).stdout
    return {line.split()[1] for line in out.splitlines() if line.startswith('#define ')}


class CpuTarget(unittest.TestCase):
    def targets(self):
        project = (REPO / 'dm/ios/project.yml').read_text()
        check = (REPO / 'dm/ios/tools/compile_check.py').read_text()
        found = {'project.yml': re.findall(r'-mcpu=([\w-]+)', project), 'compile_check.py': re.findall(r"-mcpu=([\w-]+)", check)}
        for where, cpus in found.items(): self.assertEqual(len(cpus), 1, f'{where}: one -mcpu, found {cpus}')
        return {where: cpus[0] for where, cpus in found.items()}

    def test_ios_target_promises_no_feature_the_oldest_supported_chip_lacks(self):
        if not shutil.which('xcrun'): self.skipTest('Xcode required')
        for where, cpu in self.targets().items():
            with self.subTest(where=where, cpu=cpu):
                m = macros(cpu)
                for feature in NEEDED: self.assertIn(feature, m, f'{where}: -mcpu={cpu} lacks {feature} (LSE), which the host assembly uses')
                for feature in ABSENT: self.assertNotIn(feature, m, f'{where}: -mcpu={cpu} promises {feature}, which an M1 lacks')

    def test_both_builds_use_the_same_target(self):
        self.assertEqual(len(set(self.targets().values())), 1)


if __name__ == '__main__': unittest.main()
