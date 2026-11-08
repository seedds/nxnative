#!/usr/bin/env python3
"""BarrierExits.cs: a compute thread that exits before a barrier its threadgroup still runs keeps arriving at the barriers instead of returning. NVIDIA's BAR counts
exited threads as arrived; Metal needs every thread of a threadgroup at each threadgroup_barrier, and a kernel that returns early on some threads can leave its command
buffer running for ever (PRAGMATA's "if (lid * 4 >= count) return; barrier" compute shaders). Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import json, os, re, struct, subprocess, sys, tempfile, unittest
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


# Maxwell (the pass runs on every compute shader): S2R R1, SR_TID.X; ISETP.GE.U32.AND P0, PT, R1, 0x20, PT; @P0 EXIT; IADD.CC R3, R1, R1; BAR.SYNC 0; STS [0x8], R3.
# The IADD.CC writes the condition flags, the overflow one too
MW_PT, MW_RZ, MW_SCHED, MW_NOP = 7, 255, 0x001f8000fc0007e0, 0x50b0000000070f00
def mw_op(top, shift, **f): return top << shift | MW_PT << 16 | sum(v << dict(rd=0, a=8, b=20)[k] for k, v in f.items())
MW_CODE = [mw_op(0xf0c8, 48) | 0x21 << 20 | 1,                                                    # S2R R1, SR_TID.X
           0x366 << 52 | 6 << 49 | MW_PT << 39 | 0x20 << 20 | MW_PT << 16 | 1 << 8 | 0 << 3 | MW_PT,   # ISETP.GE.U32.AND P0, PT, R1, 0x20, PT
           0xe30000000000000f,                                                                     # @P0 EXIT
           mw_op(0x5c1, 52, rd=3, a=1, b=1) | 1 << 47,                                             # IADD.CC R3, R1, R1
           0xf0a81b8000070000,                                                                     # BAR.SYNC 0x0
           mw_op(0xef58, 48, rd=3, a=MW_RZ) | 4 << 48 | 8 << 20,                                   # STS [RZ + 0x8], R3
           0xe30000000007000f]                                                                     # EXIT


@unittest.skipUnless(__import__('os').environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class MaxwellBarrierExits(unittest.TestCase):
    def kernel(self):
        code = b''.join(struct.pack('<4Q', MW_SCHED, *(tuple(MW_CODE[k:k + 3]) + (MW_NOP, MW_NOP))[:3]) for k in range(0, len(MW_CODE), 3))
        tools = Path(__file__).resolve().parents[1] / 'tools'
        with tempfile.TemporaryDirectory(prefix='barrier_exits_') as d:
            work = Path(d); (work / 'k.bin').write_bytes(code)
            (work / 'jobs.json').write_text(json.dumps([dict(key='k', stage='cs', code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={}, local=[64, 1, 1], debug=True, shared=0x1000)]))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), d], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            sys.path.insert(0, str(tools)); import dmsh_geometry
            dmsh_geometry.flatten_sets(work / 'k.spv')
            r = subprocess.run([str(tools / 'spirv_cross_bin/spirv-cross'), '--msl', '--msl-version', '20300', '--msl-argument-buffers', str(work / 'k.spv')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            return r.stdout[r.stdout.index('kernel void'):]

    def test_the_exit_flag_is_not_a_condition_flag(self):
        # The flag the threads test after the barrier holds only "exited or not": with Maxwell's overflow flag as its register, the IADD.CC overwrote it with its overflow, so a
        # live thread whose addition overflowed returned at the barrier without its store
        body = self.kernel()
        after = body[body.index('threadgroup_barrier'):]
        cond = re.search(r'if \((\w+)\)\s*\{\s*return;', after)
        self.assertIsNotNone(cond, after)
        names, values = {cond.group(1)}, []
        for _ in range(3):   # through the copies SPIRV-Cross makes of the phi
            for n in list(names): names |= set(re.findall(r'\b%s = (\w+);' % n, body)) | set(re.findall(r'bool %s = (\w+);' % n, body))
        for n in names: values += re.findall(r'\b%s = ([^;]+);' % n, body)
        self.assertTrue(values, body)
        self.assertTrue(all(v in ('true', 'false') or v in names for v in values), f'the exit flag takes a computed value: {values}\n{body}')


if __name__ == '__main__':
    unittest.main()
