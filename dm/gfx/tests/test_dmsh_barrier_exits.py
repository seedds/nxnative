#!/usr/bin/env python3
"""BarrierExits.cs: a compute thread that exits before a barrier its threadgroup still runs keeps arriving at the barriers instead of returning. NVIDIA's BAR counts
exited threads as arrived; Metal needs every thread of a threadgroup at each threadgroup_barrier, and a kernel that returns early on some threads can leave its command
buffer running for ever (PRAGMATA's "if (lid * 4 >= count) return; barrier" compute shaders). Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import re, sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_dmsh_ampere_mem import EXIT, Translated, inst

S2R_R2 = (0x000e260000002100, 0x0000000000027919)       # S2R R2, SR_TID.X
ISETP_P0 = (0x001fe20003f06070, 0x000000200200780c)     # ISETP.GE.AND P0, PT, R2, 0x20, PT (waits for the S2R)
EXIT_P0 = (EXIT[0], (EXIT[1] & ~0xf000) | 0x0 << 12)     # @P0 EXIT
BAR = (0x000fec0000010000, 0x0000000000007b1d)          # BAR.SYNC.DEFER_BLOCKING 0x0
STS = (0x000fe8000000c800, 0x0000080202008388)          # STS.U32 [R2 * 16 + 8], R2


@unittest.skipUnless(__import__('os').environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class BarrierExits(Translated):
    def kernel(self, *code):
        msl = self.translate(b''.join(inst(*i) for i in code) + inst(*EXIT), shared=0x1000)
        return msl[msl.index('kernel void'):]

    def test_an_exit_before_a_barrier_keeps_the_thread_at_the_barrier(self):
        body = self.kernel(S2R_R2, ISETP_P0, EXIT_P0, BAR, STS)
        self.assertIn('threadgroup_barrier', body)
        self.assertNotRegex(body[:body.index('threadgroup_barrier')], r'\breturn;', 'a thread returns before the barrier')

    def test_no_barrier_after_the_exit_keeps_the_return(self):
        body = self.kernel(S2R_R2, BAR, ISETP_P0, EXIT_P0, STS)
        self.assertRegex(body, r'\breturn;')


if __name__ == '__main__':
    unittest.main()
