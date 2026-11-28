"""Fixed storage-buffer pointer sources through the real translator and reflection.
Discovery is metadata only: scalar and vector accesses keep the raw pool table.
"""
import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_dmsh_storage_offset as storage_offset  # noqa: E402
from test_dmsh_storage_offset import ldg, mov_c  # noqa: E402
from test_dmsh_maxwell_ftz import EXIT, code, sts, s2r_tid, fsetp  # noqa: E402
from dmsh_reflect import reflect  # noqa: E402


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class RawPointers(unittest.TestCase):
    def test_mixed_traced_and_loaded_pointer_paths(self):
        def predicated_mov(rd, src): return 0x5c98 << 48 | 0xf << 39 | src << 20 | rd  # @P0 MOV rd, src
        body = (s2r_tid(1), fsetp(1, 255), mov_c(2, 0, 0x310), mov_c(3, 0, 0x314), ldg(4, 2), ldg(5, 2) | 4 << 20,
                mov_c(6, 0, 0x310), mov_c(7, 0, 0x314), predicated_mov(6, 4), predicated_mov(7, 5), ldg(8, 6), sts(8, 0), EXIT)
        res, msl = storage_offset.StorageOffset.translate(self, dict(stage='cs', shared=0x1000), code(*body))
        self.assertTrue(any('some paths only' in n for n in res['notes']))
        table = [b for b in reflect('k', 'cs', msl, res) if b['kind'] == 'gmem_map']
        self.assertEqual(table[0]['pointers'], [[0, 0]])

    def test_scalar_and_vector_loads_discover_the_same_pointer(self):
        for words, size in ((1, 4), (2, 5), (4, 6)):
            with self.subTest(words=words):
                # c0 + 0x310 is compute storage slot 0. Its words at bytes 8 and 12 hold the address of a second, otherwise untraceable buffer.
                raw = (ldg(8, 4) & ~(7 << 48)) | size << 48
                body = (mov_c(2, 0, 0x310), mov_c(3, 0, 0x314), ldg(4, 2) | 8 << 20, ldg(5, 2) | 12 << 20, raw,
                        *(sts(8 + k, k * 4) for k in range(words)), EXIT)
                res, msl = storage_offset.StorageOffset.translate(self, dict(stage='cs', shared=0x1000), code(*body))
                bindings = reflect('k', 'cs', msl, res)
                self.assertEqual([b['kind'] for b in bindings], ['sblen', 'sbuf', 'gmem_map'])
                self.assertEqual(bindings[2]['pointers'], [[0, 8]])
                self.assertEqual(len([n for n in res['notes'] if 'gptr site ' in n]), 1)
                if words > 1:
                    self.assertIn(f'reinterpret_cast<const device uint{words}*>', msl)


if __name__ == '__main__':
    unittest.main()
