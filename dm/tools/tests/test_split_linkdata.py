"""shader_split: a LINKDATA archive is read through the index beside it, under each name KTGL games use (Three Houses' DATA1.bin + DATA0.bin, Pokemon Pokopia's LD_C0_R.BIN + LI_C0_R.IDX,
DRAGON QUEST BUILDERS 2's LINKDATA.BIN + LINKDATA.IDX), and its compressed entries are scanned one by one. No game files needed."""
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '0100199023B2E000'   # shader_split loads a profile at import; other tests expect no default
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had


class LinkdataIndex(unittest.TestCase):
    def test_each_pair(self):
        with tempfile.TemporaryDirectory() as d:
            for name, idx in (('DATA1.bin', 'DATA0.bin'), ('LD_C0_R.BIN', 'LI_C0_R.IDX'), ('LINKDATA.BIN', 'LINKDATA.IDX')):
                path = os.path.join(d, name)
                Path(path).touch()
                self.assertIsNone(ss.linkdata_index(path), name)   # the index is missing
                Path(d, idx).touch()
                self.assertEqual(ss.linkdata_index(path), os.path.join(d, idx))

    def test_other_names(self):
        with tempfile.TemporaryDirectory() as d:
            Path(d, 'OTHER.IDX').touch()
            self.assertIsNone(ss.linkdata_index(os.path.join(d, 'OTHER.BIN')))

    def test_jobs_cover_every_entry(self):
        with tempfile.TemporaryDirectory() as d:
            size = 0x1000
            Path(d, 'LINKDATA.IDX').write_bytes(b''.join(struct.pack('<QQQQ', i * size, size, size, 0) for i in range(8)))
            Path(d, 'LINKDATA.BIN').write_bytes(bytes(8 * size))
            jobs = ss.linkdata_jobs(os.path.join(d, 'LINKDATA.BIN'), 'LINKDATA.BIN', parts=4)
            assert jobs is not None
            self.assertEqual([j[2] for j in jobs], [(0, 2), (2, 4), (4, 6), (6, 8)])


if __name__ == '__main__':
    unittest.main()
