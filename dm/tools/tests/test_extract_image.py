"""Run the real C# image writer on synthetic LibHac RomFS storage, with no game files or keys."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
import romfs_image


@unittest.skipUnless(shutil.which('dotnet'), '.NET SDK required')
class Extraction(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(); cls.addClassCleanup(cls.tmp.cleanup)
        cls.root = Path(cls.tmp.name)
        project = cls.root / 'extract_test.csproj'
        project.write_text('<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType><TargetFramework>net10.0</TargetFramework>'
                           '<AllowUnsafeBlocks>true</AllowUnsafeBlocks><StartupObject>ExtractImageTests</StartupObject></PropertyGroup><ItemGroup>'
                           f'<Compile Include="{REPO}/dm/tools/nx_extract/Program.cs"/><Compile Include="{REPO}/dm/tools/tests/fixtures/extract_image.cs"/>'
                           '<PackageReference Include="LibHac" Version="0.19.0"/></ItemGroup></Project>')
        cls.env = dict(os.environ, DOTNET_CLI_TELEMETRY_OPTOUT='1')
        r = subprocess.run(['dotnet', 'build', str(project), '-c', 'Release', '-o', str(cls.root / 'bin'), '--nologo', '-p:NuGetAudit=false'], env=cls.env, capture_output=True, text=True, timeout=120)
        if r.returncode: raise RuntimeError(r.stdout + r.stderr)
        cls.dll = cls.root / 'bin/extract_test.dll'

    def extract(self, mode):
        out = self.root / f'{mode}.img'; out.write_bytes(b'previous image')
        r = subprocess.run(['dotnet', str(self.dll), mode, str(out)], env=self.env, capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertFalse(Path(str(out) + '.part').exists())
        return out

    def assert_contents(self, out):
        index = romfs_image.read_index(out)
        self.assertEqual(set(index), {'dir', 'dir/small', 'large', 'zero'})
        with out.open('rb') as f:
            for name, expected in [('dir/small', b'\1\2\3'), ('large', bytes((i * 17 + 3) & 255 for i in range((12 << 20) + 37))), ('zero', b'')]:
                directory, offset, size = index[name]
                self.assertFalse(directory); self.assertEqual(size, len(expected))
                f.seek(offset); self.assertEqual(f.read(size), expected)

    def test_large_file_spans_multiple_read_blocks(self):
        self.assert_contents(self.extract('clean'))

    def test_empty_image(self):
        out = self.extract('empty')
        self.assertEqual(romfs_image.read_index(out), {})
        self.assertEqual(out.stat().st_size, 56)

    def test_damage_is_reported_once_and_preserves_previous_image(self):
        for mode in ('damage', 'raw-damage'):
            with self.subTest(mode=mode):
                self.assertEqual(self.extract(mode).read_bytes(), b'previous image')

    def test_allow_damaged_rewrites_the_failed_ranges(self):
        for mode in ('allow', 'raw-allow'):
            with self.subTest(mode=mode):
                self.assert_contents(self.extract(mode))

    def test_read_failure_preserves_previous_image(self):
        self.assertEqual(self.extract('failure').read_bytes(), b'previous image')


if __name__ == '__main__': unittest.main()
