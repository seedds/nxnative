#!/usr/bin/env python3
"""Maxwell float comparisons with .FTZ through the offline translator: a denormal operand compares as zero, as on NVIDIA, where Apple's GPUs keep denormals (test_dmsh_ampere_alu.py has
Ampere's). The flag is FSETP's bit 47 (the decoder also calls it WriteCC; NAK's sm50.rs has .FTZ there), FSET's bit 55 and FCMP's bit 47. FCMP compares only its third source: the two it
chooses between pass through. Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import json, os, struct, subprocess, sys, tempfile, unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
FLUSH = '1.1754943508222875'   # FLT_MIN, the bound under which an operand is flushed
PT, RZ, NEU = 7, 255, 0xd
SCHED = 0x001f8000fc0007e0     # the scheduling word before each three instructions
NOP = 0x50b0000000070f00
EXIT = 0xe30000000007000f
SR_TID_X = 0x21


def op(top, shift, **fields):
    bit = dict(rd=0, a=8, b=20, guard=16)
    v = top << shift | PT << 16
    for k, value in fields.items(): v |= value << bit[k]
    return v


def s2r_tid(rd): return op(0xf0c8, 48) | SR_TID_X << 20 | rd
def fsetp(a, b, ftz=True): return op(0x5bb, 52, a=a, b=b) | NEU << 48 | int(ftz) << 47 | PT << 39 | 0 << 3 | PT   # FSETP.NEU.AND P0, PT, a, b, PT
def fset(rd, a, b, ftz=True): return op(0x58, 56, rd=rd, a=a, b=b) | int(ftz) << 55 | 1 << 52 | NEU << 48 | PT << 39   # FSET.BF.NEU.AND rd, a, b, PT
def fcmp(rd, a, b, c, ftz=True): return op(0x5ba, 52, rd=rd, a=a, b=b) | NEU << 48 | int(ftz) << 47 | c << 39           # FCMP.NEU rd, a, b, c
def sel(rd, a, b): return op(0x5ca, 52, rd=rd, a=a, b=b)                                                                 # SEL rd, a, b, P0: keeps the predicate
def sts(rd, offset): return op(0xef58, 48, rd=rd, a=RZ) | 4 << 48 | offset << 20                                         # STS.B32 [RZ + offset], rd: keeps the result


def code(*insts):
    out = b''
    for k in range(0, len(insts), 3):
        out += struct.pack('<4Q', SCHED, *(tuple(insts[k:k + 3]) + (NOP, NOP))[:3])
    return out


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class MaxwellCompareFtz(unittest.TestCase):
    def msl(self, *body):
        """body between S2R R1, R2, R3 = the thread index (nothing to fold) and the store of R3"""
        with tempfile.TemporaryDirectory(prefix='maxwell_ftz_') as d:
            work = Path(d); (work / 'k.bin').write_bytes(code(s2r_tid(1), s2r_tid(2), s2r_tid(3), *body, sts(3, 8), EXIT))
            (work / 'jobs.json').write_text(json.dumps([dict(key='k', stage='cs', code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={}, local=[32, 1, 1], debug=True, shared=0x1000)]))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), d], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue((work / 'k.spv').exists(), r.stdout + r.stderr)
            sys.path.insert(0, str(TOOLS)); import dmsh_geometry
            dmsh_geometry.flatten_sets(work / 'k.spv')
            r = subprocess.run([str(TOOLS / 'spirv_cross_bin/spirv-cross'), '--msl', '--msl-version', '20300', '--msl-argument-buffers', str(work / 'k.spv')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            return r.stdout

    def test_fsetp_flushes_both_operands(self):
        self.assertEqual(self.msl(fsetp(1, 2), sel(3, 1, 2)).count(FLUSH), 2)

    def test_fsetp_without_ftz_compares_as_is(self):
        self.assertNotIn(FLUSH, self.msl(fsetp(1, 2, ftz=False), sel(3, 1, 2)))

    def test_fsetp_against_zero_flushes_one(self):
        self.assertEqual(self.msl(fsetp(1, RZ), sel(3, 1, 2)).count(FLUSH), 1)

    def test_fset_flushes_both_operands(self):
        self.assertEqual(self.msl(fset(3, 1, 2)).count(FLUSH), 2)

    def test_fset_without_ftz_compares_as_is(self):
        self.assertNotIn(FLUSH, self.msl(fset(3, 1, 2, ftz=False)))

    def test_fcmp_flushes_only_what_it_compares(self):
        self.assertEqual(self.msl(fcmp(3, 1, 2, 3)).count(FLUSH), 1)

    def test_fcmp_without_ftz_compares_as_is(self):
        self.assertNotIn(FLUSH, self.msl(fcmp(3, 1, 2, 3, ftz=False)))


if __name__ == '__main__':
    unittest.main()
