#!/usr/bin/env python3
"""A hull shader that writes its control points straight into the stage's circular buffer, through the offline translator (DirectCbeWrites in the translator; docs/reference/shaders.md,
A hull shader's control points written into the stage's circular buffer). The hull below is assembled by hand in the form NVIDIA's compiler gives every such shader: the buffer's address
from system registers 21 and 22, the count of per-patch words and the stride from ISBERD with O at 0x7e and 0x7f, output word k of lane L at
address + 0x80 * (1 + ceil(N / 4) + p) + 4 * L + 4 * S * k (p by a predicated add), a copy of an output into the buffer after the body. The output words are the header's output map: position xyzw,
then generic attribute 0's x and y. Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import json, os, re, struct, subprocess, sys, tempfile, unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
PT, RZ = 7, 255
SCHED = 0x001f8000fc0007e0     # the scheduling word before each three instructions
NOP = 0x50b0000000070f00
EXIT = 0xe30000000007000f
SR_LANEID, SR_INVOCATION, SR_CBE_LO, SR_CBE_HI = 0, 17, 21, 22

def s2r(rd, sr): return 0xf0c8 << 48 | PT << 16 | sr << 20 | rd
def mov32i(rd, imm): return 0x010 << 52 | imm << 20 | 0xf << 12 | PT << 16 | rd
def mov(rd, rb): return 0x5c98 << 48 | 0xf << 39 | rb << 20 | PT << 16 | rd
def isberd(rd, ra, o=True): return 0xefd0 << 48 | int(o) << 32 | PT << 16 | ra << 8 | rd               # ISBERD rd, [ra] (O: the shader's own outputs)
def iadd32i(rd, ra, imm, guard=PT): return 0x1c0 << 52 | (imm & 0xffffffff) << 20 | guard << 16 | ra << 8 | rd
def iadd(rd, ra, rb): return 0x5c10 << 48 | rb << 20 | PT << 16 | ra << 8 | rd
def iadd3i(rd, ra, imm, rc): return 0x38c0 << 48 | rc << 39 | imm << 20 | PT << 16 | ra << 8 | rd           # IADD3 rd, ra, imm, rc
def shr(rd, ra, n): return 0x3828 << 48 | n << 20 | PT << 16 | ra << 8 | rd
def shl(rd, ra, n): return 0x3848 << 48 | n << 20 | PT << 16 | ra << 8 | rd
def xmadi(rd, ra, imm, rc): return 0x3600 << 48 | rc << 39 | imm << 20 | PT << 16 | ra << 8 | rd            # XMAD rd, ra, imm16, rc
def isetp_ne(a, b): return 0x5b6 << 52 | 5 << 49 | 1 << 48 | PT << 39 | b << 20 | PT << 16 | a << 8 | 0 << 3 | PT   # ISETP.NE.AND P0, PT, a, b, PT
def ald(rd, attr, vtx): return 0xefd8 << 48 | vtx << 39 | attr << 20 | PT << 16 | RZ << 8 | rd             # ALD.32 rd, a[attr], vtx
def ast_patch(attr, rs): return 0xeff0 << 48 | RZ << 39 | 1 << 31 | attr << 20 | PT << 16 | RZ << 8 | rs   # AST.P.32 a[attr], rs
def stg(ra, rd): return 0xeedc6 << 44 | PT << 16 | ra << 8 | rd                                           # STG.E.32 [ra], rd (the high word in ra + 1)

def code(*insts):
    out = b''
    for k in range(0, len(insts), 3):
        out += struct.pack('<4Q', SCHED, *(tuple(insts[k:k + 3]) + (NOP, NOP))[:3])
    return out

def header():
    h = bytearray(0x50)
    struct.pack_into('<5I', h, 0, 1 | 3 << 5 | 2 << 10 | 1 << 16 | 3 << 17 | 1 << 26, 6 << 24, 4 << 24, 0, 0)   # a tessellation control shader: 6 per-patch words, 4 control points
    h[0x17] = 0xf0      # inputs: position xyzw
    h[0x35] = 0xf0      # outputs: position xyzw ...
    h[0x36] = 0x03      # ... and generic attribute 0's x and y
    return bytes(h)

def hull(extra_term=False):
    body = [s2r(7, SR_LANEID), mov32i(2, 0x7e), mov32i(3, 0x7f), isberd(0, 2), isberd(4, 3), s2r(8, SR_CBE_LO), s2r(5, SR_CBE_HI),
            iadd32i(0, 0, 3), shr(9, 0, 2), shl(7, 7, 2), xmadi(6, 9, 0x80, RZ), iadd(10, 8, 6),
            s2r(12, SR_INVOCATION), isetp_ne(12, RZ), iadd32i(10, 10, 0x80, guard=0),                       # the predicated row (p)
            shl(4, 4, 2), iadd3i(24, 7, 0x80, 10), isberd(21, 12, o=False)]
    body += [ald(40 + k, 0x70 + 4 * k, 21) for k in range(4)] + [ald(44, 0x80, 21), ald(45, 0x84, 21)]
    for k in range(6):
        body += [xmadi(26, 4, k, RZ), iadd(30, 24, 26), mov(31, 5)]
        if extra_term and k == 5: body.append(iadd(30, 30, 12))                                               # an address that is not base + lane + constants
        body.append(stg(30, 40 + k))
    body += [iadd32i(1, 7, 0x7f), isberd(1, 1), iadd(30, 8, 7), mov(31, 5), stg(30, 1)]                     # the header word copied into row 0: dropped
    body += [ast_patch(4 * i, 40) for i in range(6)] + [EXIT]
    return header() + code(*body)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class HullCircularBuffer(unittest.TestCase):
    def translate(self, blob):
        with tempfile.TemporaryDirectory(prefix='hull_cbe_') as d:
            work = Path(d); (work / 'k.bin').write_bytes(blob)
            (work / 'jobs.json').write_text(json.dumps([dict(key='k', stage='hs', code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={}, debug=True)]))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), d], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue((work / 'k.spv').exists(), r.stdout + r.stderr)
            notes = json.loads((work / 'k.res.json').read_text()).get('notes', [])
            sys.path.insert(0, str(TOOLS)); import dmsh_geometry
            dmsh_geometry.flatten_sets(work / 'k.spv')
            r = subprocess.run([str(TOOLS / 'spirv_cross_bin/spirv-cross'), '--msl', '--msl-version', '20300', '--msl-argument-buffers', str(work / 'k.spv')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            return r.stdout, notes

    def test_stores_become_the_control_points_outputs(self):
        msl, notes = self.translate(hull())
        for c in 'xyzw':
            self.assertRegex(msl, rf'gl_out\[[^\]]+\]\.gl_Position\.{c} = ', c)
        self.assertEqual(len(re.findall(r'gl_out\[[^\]]+\]\.m_\d+\.[xy] = ', msl)), 2, msl)
        self.assertNotIn('reinterpret_cast<device', msl)       # no store left to an address
        self.assertTrue(any('6 stores became outputs (6 of the header\'s 6 output words), 1 copies of its outputs dropped.' in n for n in notes), notes)

    def test_each_word_gets_its_own_value(self):
        msl, _ = self.translate(hull())
        loads = dict(re.findall(r'float (_\d+) = gl_in\[[^\]]+\]\.gl_Position\.([xyzw]);', msl))
        stores = dict((c, v) for c, v in re.findall(r'gl_out\[[^\]]+\]\.gl_Position\.([xyzw]) = (_\d+);', msl))
        self.assertEqual({c: loads.get(v) for c, v in stores.items()}, {c: c for c in 'xyzw'}, msl)

    def test_an_address_with_another_term_is_not_guessed(self):
        msl, notes = self.translate(hull(extra_term=True))
        self.assertTrue(any('5 stores became outputs' in n and '1 stores not decoded' in n for n in notes), notes)
        self.assertTrue(any('not the base plus a lane term and constants' in n for n in notes), notes)
        self.assertNotIn('reinterpret_cast<device', msl)


if __name__ == '__main__':
    unittest.main()
