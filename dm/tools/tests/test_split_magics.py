"""shader_split: what a scan reads or starts once instead of once per finder or per file. The four job finders that know their archives by the magic (CRA, zip, PDAT, RPF6) are
given the files that start with theirs, every file's magic read once for all four, so an archive is still taken whatever its name; and a zstd file without a dictionary is
decompressed in the process, every frame of it, with no zstd command to start. No game files needed."""
import os
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
TOOLS = REPO / 'dm/gfx/tools'
sys.path.insert(0, str(REPO / 'dm/tools/fuzz'))
import fuzz_shader_split as fz  # noqa: E402

ss = fz.ss


def stage(sph_type, word):   # fz.stage with other instruction words: another shader, another key
    ctl, code = fz.stage(sph_type)
    return ctl, code[:0x80] + bytes.fromhex(word) * 8


class Magics(unittest.TestCase):
    def test_the_first_four_bytes_of_each_file_in_order(self):
        with tempfile.TemporaryDirectory() as t:
            paths = []
            for name, data in (('b', b'PK\x03\x04rest'), ('a', b'PD'), ('c', b'')):
                (Path(t) / name).write_bytes(data); paths.append(str(Path(t) / name))
            self.assertEqual(ss.file_magics(paths), [b'PK\x03\x04', b'PD', b''])


class Split(unittest.TestCase):
    def test_an_archive_by_its_magic_and_a_zstd_file_of_two_frames(self):   # the whole split, with no zstd command to find
        with tempfile.TemporaryDirectory() as t:
            t = Path(t); rom = t / 'RomFS'; (rom / 'data').mkdir(parents=True); (rom / 'Shaders').mkdir()
            with zipfile.ZipFile(rom / 'data/pack.bin', 'w', zipfile.ZIP_DEFLATED) as z: z.writestr('program.glslc', fz.glslc([fz.VS, fz.PS]))   # deflated: a loose scan of the file finds nothing
            other = fz.glslc([stage(0x0461, '0f00000000000001'), stage(0x1462, '0f00000000000002')])
            zstd = ss.zstd_mod()
            (rom / 'Shaders/two.zs').write_bytes(zstd.compress(other[:0x400]) + zstd.compress(other[0x400:]))
            main = t / 'main'; main.write_bytes(b'NSO0' + bytes(0xfc))   # an NSO header of three empty segments
            empty = t / 'bin'; empty.mkdir()
            out = [t / n for n in ('pkg', 'map.tsv', 'facts.tsv', 'pairs.tsv', 'pso.tsv')]
            r = subprocess.run([sys.executable, TOOLS / 'shader_split.py', rom, main, *out], capture_output=True, text=True, env=dict(os.environ, DM_GAME='01007BB017812000', J='2', PATH=str(empty)))
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertNotIn('[RED]', r.stdout)
            sources = sorted(ln.split('\t')[4] for ln in out[1].read_text().splitlines() if not ln.startswith('#') and ln.split('\t')[0].endswith('.bin'))
            self.assertEqual(len(sources), 4, sources)
            self.assertEqual([s.startswith('/Shaders/two.zs') for s in sources], [True, True, False, False])
            self.assertEqual([s.startswith('/data/pack.bin/program.glslc') for s in sources], [False, False, True, True])


if __name__ == '__main__':
    unittest.main()
