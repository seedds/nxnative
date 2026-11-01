"""LibHac reads a missing final PFS0 name terminator through a read-only header view.
Synthetic packages only: no keys or game data, and the production nx_extract source is compiled unchanged.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[3]
DOTNET = shutil.which('dotnet')


@unittest.skipUnless(DOTNET, '.NET SDK 10 is required')
class Pfs0Storage(unittest.TestCase):
    def test_read_only_header_and_payload(self):
        assert DOTNET is not None
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            package = ET.parse(ROOT / 'dm/tools/nx_extract/nx_extract.csproj').find('.//PackageReference[@Include="LibHac"]')
            assert package is not None
            version = package.attrib['Version']
            (tmp / 'test.csproj').write_text(f'''<Project Sdk="Microsoft.NET.Sdk">
<PropertyGroup><OutputType>Exe</OutputType><TargetFramework>net10.0</TargetFramework><AllowUnsafeBlocks>true</AllowUnsafeBlocks><StartupObject>Pfs0StorageTests</StartupObject></PropertyGroup>
<ItemGroup><PackageReference Include="LibHac" Version="{escape(version)}" />
<Compile Include="{escape(str(ROOT / 'dm/tools/nx_extract/Program.cs'))}" />
<Compile Include="{escape(str(ROOT / 'dm/tools/tests/fixtures/Pfs0StorageTests.cs'))}" /></ItemGroup></Project>''')
            result = subprocess.run([DOTNET, 'run', '--project', str(tmp / 'test.csproj'), '-v:q'],
                                    capture_output=True, text=True, timeout=120, env=dict(os.environ, DOTNET_NOLOGO='1'))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PFS0 storage checks passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
