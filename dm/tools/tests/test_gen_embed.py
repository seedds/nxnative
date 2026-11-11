import pathlib
import subprocess
import sys
import tempfile
import unittest

TOOL = pathlib.Path(__file__).resolve().parents[2] / 'ios/tools/gen_embed.py'


class EmbedHeaders(unittest.TestCase):
    def test_relay_metadata_does_not_change_embedded_layout(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / 'main.patched.bin').write_bytes(bytes(0x10000))
            header = root / 'main.fixups.tsv'
            header.write_text('# TEXT_END\t0x4000\n# PATCH_OFF\t-0x10000\n# KEY_PAGE_OFF\t0x10000\n# KEY_PAGE_SIZE\t0x4000\n# FILE_IMAGE_LEN\t0x8000\n# BACK_LEN\t0x4000\n')
            output = root / 'embed.S'
            command = [sys.executable, str(TOOL), str(root), str(output), 'main']
            first = subprocess.run(command, capture_output=True, text=True, check=True)
            baseline = output.read_text()
            with header.open('a') as file:
                file.write('# BRANCH_RELAY:163155fc:0\t0xe000000\t0xd9bfd28\n')
            second = subprocess.run(command, capture_output=True, text=True, check=True)
            self.assertEqual(output.read_text(), baseline)
            self.assertEqual(second.stdout, first.stdout)
            self.assertEqual(baseline.count('.section'), 4)
            self.assertIn('__TEXT,__dmmainx2', baseline)


if __name__ == '__main__':
    unittest.main()
