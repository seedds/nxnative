#!/usr/bin/env python3
"""Ampere instructions Fire Emblem Fortune's Weave's shaders brought to the offline translator (dmsh_xlate, arch sm86): BREV, shared-memory atomics (ATOMS), MEMBAR, F2I to and I2F
from a 64-bit integer, IMAD.WIDE / IMAD.HI with a carry out, IMAD.HI's pair addend, IMAD.X's carry in, ERRBAR, and the stride of shared-memory addresses. The encodings are NAK's (Mesa) fields, the operands chosen here; each result goes into a shared-memory atomic, so
none is dropped as dead. Every instruction waits on every scoreboard (116..122): the translator gives a variable-latency result (S2R, the conversions) only to an instruction
that waits for it, as the hardware does. Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import json, os, re, struct, subprocess, sys, tempfile, unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
SCHED = 0x000fca0000000000   # scheduling bits only (no operands above bit 63)
EXIT = (0x000fea0003800000, 0x000000000000794d)
S2R_TID = (0x000e220000002100, 0x0000000000007919)        # S2R R0, SR_TID.X (0x21 at 72)
BREV = (SCHED, 0x0000000000017301)                         # BREV R1, R0
I2FP = (SCHED, 0x0000000000037245)                         # I2FP.F32.U32 R3, R0
F2I_U64 = (0x000e30000021d800, 0x0000000300047311)         # F2I.U64.F32.TRUNC R4, R3: destination size 3 (75..77), source size 2 (84..86)
I2F_U64 = (0x000e220000301000, 0x0000000400067312)         # I2F.F32.U64 R6, R4: source size 3, destination size 2
IMAD_WIDE = (0x000fc80007800004, 0x0000000000087225)       # IMAD.WIDE.U32 R8, P0, R0, R0, R4: P0 at 81..84
IMAD_HI = (0x000fc80007820004, 0x00000000000a7227)         # IMAD.HI.U32 R10, P1, R0, R0, R4
IMAD_WIDE_C0 = (0x000fc80007800000, 0x0000000000087225)    # IMAD.WIDE.U32 R8, P0, R0, R0, R0
WIDE_NO_C = (0x000fc800078e00ff, 0x0000000000087225)       # IMAD.WIDE.U32 R8, R0, R0, RZ
HI_PAIR = (0x000fc800078e0008, 0x00000000000a7227)         # IMAD.HI.U32 R10, R0, R0, R8: C is the pair R9:R8
IMAD_X = (0x000fc800000e0409, 0x00000000000b7224)          # IMAD.X R11, R0, R0, R9, P0: .X (74), the carry predicate at 87..90
MEMBAR = (0x000fec0000000000, 0x0000000000007992)          # MEMBAR.CTA
MEMBAR_GPU = (0x000fec0000002000, 0x0000000000007992)      # MEMBAR.GPU (scope 2 at 76..79)
ERRBAR = (0x000fc00000000000, 0x00000000000079ab)          # ERRBAR: no operands, after a device-scope MEMBAR
STS_X4 = (0x000fe80000004800, 0x0000000000007388)          # STS [R0.X4], R0: 32-bit (4 at 73..76), stride x4 (1 at 78..80), the game's auto-exposure clear
LDS_X4 = (0x000e280000004800, 0x0000000000057984)          # LDS R5, [R0.X4]


def atoms(rd, rb, off):   # ATOMS.ADD.U32 Rd, [RZ + off], Rb
    return (0x000fe20000000000, off << 40 | rb << 32 | 0xff << 24 | rd << 16 | 0x738c)


WAIT_ALL = 0x3f << 52   # wait mask 116..122


def inst(hi, lo): return struct.pack('<QQ', lo, hi | WAIT_ALL)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class AmpereOps(unittest.TestCase):
    def translate(self, *code):
        with tempfile.TemporaryDirectory(prefix='ampere_ops_') as d:
            work = Path(d); (work / 'k.bin').write_bytes(b''.join(inst(*c) for c in code) + inst(*EXIT))
            job = [dict(key='k', stage='cs', arch='sm86', code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={}, local=[32, 1, 1], shared=64, debug=True,
                        nvn2_cs=[0, 0, 0, 0x10, 0])]
            (work / 'jobs.json').write_text(json.dumps(job))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), d], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue((work / 'k.spv').exists(), r.stdout + r.stderr)
            sys.path.insert(0, str(TOOLS)); import dmsh_geometry
            dmsh_geometry.flatten_sets(work / 'k.spv')
            r = subprocess.run([str(TOOLS / 'spirv_cross_bin/spirv-cross'), '--msl', '--msl-version', '20300', '--msl-argument-buffers', str(work / 'k.spv')],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            return r.stdout

    def test_brev_into_a_shared_atomic(self):
        msl = self.translate(S2R_TID, BREV, atoms(2, 1, 0), MEMBAR)
        self.assertRegex(msl, r'reverse_bits\(as_type<int>\(_\d+\)\)')   # of the thread index (S2R), not a constant
        self.assertRegex(msl, r'atomic_fetch_add_explicit\(\(threadgroup')

    def test_64_bit_conversions(self):   # F2I.U64 splits at 2^32 (a floor of the scaled value), I2F.U64 joins the pair back
        msl = self.translate(S2R_TID, I2FP, F2I_U64, I2F_U64, atoms(12, 6, 0), atoms(13, 4, 4), atoms(14, 5, 8))
        self.assertIn('floor', msl)
        self.assertIn('4294967296', msl.replace('.0', ''))
        self.assertNotIn(', 0u, memory_order', msl)   # no result folded to a constant

    def test_shared_stride(self):   # [R0.X4]: the index scaled to a byte address, then back to a word (index R0, not R0 / 4)
        msl = self.translate(S2R_TID, STS_X4, LDS_X4, atoms(12, 5, 0))
        self.assertRegex(msl, r'<< 2')

    def test_multiply_add_with_carry_outs(self):
        msl = self.translate(S2R_TID, IMAD_WIDE, IMAD_HI, atoms(12, 8, 0), atoms(13, 9, 4), atoms(14, 10, 8))
        self.assertNotIn(', 0u, memory_order', msl)

    def test_imad_hi_adds_a_register_pair(self):   # the high word of a * b + {R9:R8}: IMAD.WIDE's high word (a mulhi) joins IMAD.HI's, with the low words' carry
        msl = self.translate(S2R_TID, WIDE_NO_C, HI_PAIR, atoms(12, 10, 0))
        mulhi = set(re.findall(r'uint (_\d+) = \(spvMulExtended<[^>]*>\([^;]*\)\)\._m1;', msl))
        self.assertEqual(len(mulhi), 2, msl)
        self.assertTrue(any({a, b} == mulhi for a, b in re.findall(r'int\((_\d+)\) \+ int\((_\d+)\)', msl)), msl)
        self.assertRegex(msl, r'bool _\d+ = uint\(_\d+\) < uint\(_\d+\);')

    def test_imad_x_adds_its_carry(self):   # IMAD.X after IMAD.WIDE's carry out: the carry (P0) is added, not dropped
        msl = self.translate(S2R_TID, IMAD_WIDE_C0, IMAD_X, atoms(12, 11, 0))
        p0 = re.search(r'bool (_\d+) = _\d+ != 0;', msl).group(1)   # IMAD.WIDE's carry out
        cin = re.search(rf'int (_\d+) = int\({p0}\);', msl).group(1)
        self.assertRegex(msl, rf'int (_\d+) = _\d+ \+ {cin};\n.*uint\(\1\), memory_order')

    def test_errbar_after_a_fence(self):   # MEMBAR.GPU then ERRBAR, the compiler's device fence: a barrier, and nothing for the error barrier
        msl = self.translate(S2R_TID, BREV, MEMBAR_GPU, ERRBAR, atoms(2, 1, 0))
        self.assertIn('threadgroup_barrier', msl.replace('simdgroup_barrier', 'threadgroup_barrier'))
        self.assertRegex(msl, r'reverse_bits\(as_type<int>\(_\d+\)\)')


if __name__ == '__main__':
    unittest.main()
