"""Shared archive probes and in-process zstd decoding, using synthetic RomFS data."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

from dm.tools.tests.test_split_gzip import fz, ss
from dm.tools.tests.test_easf import compute
import romfs_image

REPO = Path(__file__).resolve().parents[3]


def stage(original, word):
    ctl, code = original
    return ctl, code[:0x80] + bytes.fromhex(word) * 8


class Magics(unittest.TestCase):
    def test_reads_eight_bytes_in_path_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            paths = [Path(tmp) / name for name in ('b', 'a', 'c')]
            for path, data in zip(paths, (b'NXSHADERrest', b'PD', b'')):
                path.write_bytes(data)
            self.assertEqual(ss.file_heads(paths), [b'NXSHADER', b'PD', b''])

    def test_split_reads_archives_by_magic_and_joins_zstd_frames(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            romfs = root / 'RomFS'
            romfs.mkdir()
            with zipfile.ZipFile(romfs / 'pack.bin', 'w', zipfile.ZIP_DEFLATED) as archive:
                archive.writestr('program.glslc', fz.glslc([fz.VS, fz.PS]))
            (romfs / 'ea.bin').write_bytes(compute())
            other = fz.glslc([stage(fz.VS, '0f00000000000001'), stage(fz.PS, '0f00000000000002')])
            zstd = ss.zstd_mod()
            (romfs / 'two.zs').write_bytes(zstd.compress(other[:0x400]) + zstd.compress(other[0x400:]))
            image = root / 'RomFS.img'
            romfs_image.pack(romfs, image)
            main = root / 'main'
            main.write_bytes(b'NSO0' + bytes(0xfc))
            empty_bin = root / 'bin'
            empty_bin.mkdir()
            output = [root / name for name in ('pkg', 'map.tsv', 'facts.tsv', 'pairs.tsv', 'pso.tsv')]
            result = subprocess.run([sys.executable, REPO / 'dm/gfx/tools/shader_split.py', image, main, *output],
                                    env=dict(os.environ, DM_GAME='01007BB017812000', J='2', PATH=str(empty_bin)),
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn('[RED]', result.stdout)
            rows = [line.split('\t') for line in output[1].read_text().splitlines() if not line.startswith('#')]
            sources = [row[4] for row in rows if row[0].endswith('.bin')]
            self.assertEqual(len(sources), 5, sources)
            for prefix, count in (('/two.zs', 2), ('/pack.bin/program.glslc', 2), ('ea.bin#NXSHADER', 1)):
                self.assertEqual(sum(source.lstrip('/').startswith(prefix.lstrip('/')) for source in sources), count, sources)


if __name__ == '__main__':
    unittest.main()
