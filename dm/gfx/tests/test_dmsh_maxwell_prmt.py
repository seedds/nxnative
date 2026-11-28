"""Maxwell PRMT's encoded operand forms and byte modes through the production chain.

The byte/sign oracle is NVIDIA PTX's prmt table (sm_20+). Maxwell field positions
come from InstDecoders and the original MaxAs/Envytools gm107 encoders, not Ampere.
GPU cases use shared-memory inputs so selectors cannot fold; the existing Orin
runner only supplies IO and the chain rewrites, not instruction expectations.
"""
import json
import os
from pathlib import Path
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_dmsh_maxwell_ftz import EXIT, RZ, code, s2r_tid
from test_dmsh_ampere_orin import RUNNER, chain_rewrites, run_op

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import dmsh_chain
import dmsh_geometry

# PTX table, low output byte first; specialized modes never sign-extend.
SELECT = (
    (),
    ((0, 1, 2, 3), (1, 2, 3, 4), (2, 3, 4, 5), (3, 4, 5, 6)),
    ((0, 7, 6, 5), (1, 0, 7, 6), (2, 1, 0, 7), (3, 2, 1, 0)),
    ((0, 0, 0, 0), (1, 1, 1, 1), (2, 2, 2, 2), (3, 3, 3, 3)),
    ((0, 1, 2, 3), (1, 1, 2, 3), (2, 2, 2, 3), (3, 3, 3, 3)),
    ((0, 0, 0, 0), (0, 1, 1, 1), (0, 1, 2, 2), (0, 1, 2, 3)),
    ((0, 1, 0, 1), (2, 3, 2, 3), (0, 1, 0, 1), (2, 3, 2, 3)),
)


def expected(a, selector, b, mode):
    src = struct.pack('<II', a, b)
    picks = SELECT[mode][selector & 3] if mode else [(selector >> (4 * i)) & 15 for i in range(4)]
    out = bytes((255 if src[s & 7] & 128 else 0) if s & 8 else src[s] for s in picks)
    return int.from_bytes(out, 'little')


def mov(rd, value): return 0x010000000007f000 | rd | (value & 0xffffffff) << 20

def prmt(form, mode, rd=4, a=1, selector=2, b=3, pred=7, inv=False):
    # PRMT Rd, Ra, selector, Rb. Rc puts the selector in bit39 and data in c[3][20].
    top = {'r': 0x5bc, 'i': 0x36c, 'c': 0x4bc, 'rc': 0x53c}[form] << 52
    fields = rd | a << 8 | pred << 16 | int(inv) << 19 | mode << 48
    if form == 'r': fields |= selector << 20 | b << 39
    elif form == 'i': fields |= (selector & 0x7ffff) << 20 | (selector & 0x80000) << 37 | b << 39
    elif form == 'c': fields |= 3 << 34 | 5 << 20 | b << 39
    else: fields |= 3 << 34 | 5 << 20 | selector << 39
    return top | fields


def program(form, mode, selector=2, rd=4, a=1, b=3, pred=7, inv=False):
    body = [s2r_tid(9), 0x3848000000070008 | 9 << 8 | 4 << 20]  # SHL R8, R9, 4
    body += [0xef4c000000070000 | reg | 8 << 8 | off << 20 for reg, off in ((1, 0), (2, 4), (3, 8), (4, 12))]
    if pred != 7:
        body += [0x5b60000000070007 | 9 << 8 | RZ << 20 | 2 << 49 | 7 << 39]  # ISETP.EQ.U32.AND P0, PT, R9, RZ, PT
    body += [prmt(form, mode, rd, a, selector, b, pred, inv),
             0xef5c000000070000 | (4 if rd == RZ else rd) | 8 << 8 | 12 << 20, EXIT]
    return code(*body)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the translator')
class MaxwellPrmt(unittest.TestCase):
    def translate(self, instructions, refused=False):
        with tempfile.TemporaryDirectory(prefix='maxwell_prmt_') as tmp:
            work = Path(tmp)
            (work / 'k.bin').write_bytes(instructions)
            (work / 'jobs.json').write_text(json.dumps([dict(key='k', stage='cs', arch='sm50', code=str(work / 'k.bin'),
                tex={}, img={}, attr={}, fout={}, local=[32, 1, 1], debug=True, reduced=True, shared=512)]))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), tmp], capture_output=True, text=True)
            if refused:
                self.assertFalse((work / 'k.spv').exists(), r.stdout + r.stderr)
                return ''
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue((work / 'k.spv').exists(), r.stdout + r.stderr)
            dmsh_geometry.flatten_sets(work / 'k.spv')
            r = subprocess.run([dmsh_chain.SPIRV_CROSS, *dmsh_chain.CROSS_MSL, str(work / 'k.spv')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            return r.stdout

    def folded(self, form, mode, sel, rd=4, a=1, b=3, pred=7, inv=False):
        insts = [mov(1, 0x807f0180), mov(2, sel), mov(3, 0x55aa00ff), mov(4, 0xdeadbeef),
                 prmt(form, mode, rd, a, sel if form == 'i' else 2, b, pred, inv),
                 0xef5c000000070000 | rd | RZ << 8, EXIT]
        msl = self.translate(code(*insts))
        values = re.findall(r'\[0\] = (\d+)u;', msl)
        self.assertTrue(values, msl)
        return int(values[-1])

    def test_generic_literals_and_sign_replication(self):
        for form in ('r', 'i'):
            for sel, want in ((0x3210, 0x807f0180), (0x7654, 0x55aa00ff), (0x5410, 0x00ff0180),
                              (0xba98, 0xff0000ff), (0xfedc, 0x00ff00ff), (0xffff, 0)):
                with self.subTest(form=form, selector=hex(sel)):
                    self.assertEqual(self.folded(form, 0, sel), want)

    def test_specialized_modes_and_high_selector_bits(self):
        for form in ('r', 'i'):
            for mode in range(1, 7):
                for sel in range(4):
                    with self.subTest(form=form, mode=mode, selector=sel):
                        self.assertEqual(self.folded(form, mode, sel | 0xfffc), expected(0x807f0180, sel, 0x55aa00ff, mode))

    def test_signed_immediate_only_uses_low_selector_bits(self):
        self.assertEqual(self.folded('i', 0, 0xfffff), 0)
        self.assertEqual(self.folded('i', 1, 0x80003), 0xaa00ff80)

    def test_destination_aliases_sources(self):
        for rd in (1, 2, 3):
            with self.subTest(rd=rd): self.assertEqual(self.folded('r', 0, 0x5410, rd=rd), 0x00ff0180)

    def test_zero_register_sources_and_false_guard(self):
        self.assertEqual(self.folded('r', 0, 0x3210, a=RZ), 0)
        self.assertEqual(self.folded('r', 0, 0x7654, b=RZ), 0)
        self.assertEqual(self.folded('r', 0, 0x5410, inv=True), 0xdeadbeef)

    def test_reserved_modes_are_rejected(self):
        for form in ('r', 'i', 'c', 'rc'):
            for mode in (7, 8, 15):
                with self.subTest(form=form, mode=mode): self.translate(program(form, mode), refused=True)

    @unittest.skipUnless(shutil.which('xcrun'), 'needs Metal compilers')
    def test_iphone_compiles_every_form_and_mode(self):
        sys.path.insert(0, str(TOOLS.parents[1] / 'ios/tools'))
        import ios_metal
        with tempfile.TemporaryDirectory(prefix='maxwell_prmt_ios_') as tmp:
            work = Path(tmp)
            for form in ('r', 'i', 'c', 'rc'):
                for mode in range(7):
                    with self.subTest(form=form, mode=mode):
                        (work / 'k.metal').write_text(self.translate(program(form, mode)))
                        r = subprocess.run([*ios_metal.METAL, str(work / 'k.metal'), '-o', str(work / 'k.air')], capture_output=True, text=True)
                        self.assertEqual(r.returncode, 0, r.stderr)

    @unittest.skipUnless(os.environ.get('DMSH_TEST_GPU') and sys.platform == 'darwin' and shutil.which('xcrun'), 'set DMSH_TEST_GPU=1 on Mac')
    def test_runtime_operands_all_forms_and_modes(self):
        rng = random.Random(2040)
        rows = [[rng.getrandbits(32), s, rng.getrandbits(32)] for s in range(65536)]
        with tempfile.TemporaryDirectory(prefix='maxwell_prmt_gpu_') as tmp:
            work = Path(tmp)
            (work / 'run.m').write_text(RUNNER)
            subprocess.run(['clang', '-fobjc-arc', '-O1', str(work / 'run.m'), '-framework', 'Metal', '-framework', 'Foundation', '-o', str(work / 'run')], check=True, capture_output=True)
            steps, xc = chain_rewrites()
            for form in ('r', 'i', 'c', 'rc'):
                for mode in range(7):
                    with self.subTest(form=form, mode=mode):
                        inputs = rows if form in ('r', 'rc') and mode == 0 else rows[:128]
                        # Runtime high bits may not affect the selectors in specialized modes.
                        if mode: inputs = [[a, s | 0xffff0000, b] for a, s, b in inputs]
                        fixed = 0x8000fedc if mode == 0 else 0xffff0003
                        sel = fixed & 0xfffff if form == 'i' else 2
                        op: dict = dict(name=f'{form}_{mode}', arch='sm50', program=program(form, mode, selector=sel),
                                  ins=['u', 'u', 'u'], outs=['u'], rows_in=inputs)
                        if form in ('c', 'rc'): op['cbuf'] = (3, 20, fixed)
                        want = [[expected(a, fixed if form in ('i', 'c') else s, fixed if form == 'rc' else b, mode)] for a, s, b in inputs]
                        stage, got = run_op(op, work, work / 'run', steps, xc, dict(os.environ))
                        self.assertEqual(stage, 'ok', got)
                        mismatch = next(((i, x, y) for i, (x, y) in enumerate(zip(got, want)) if x != y), None)
                        self.assertIsNone(mismatch, mismatch)
            # A runtime predicate is true only in lane 0; a false guard and RZ destination retain R4.
            inputs = rows[:128]
            for rd, pred, inv, a, b in ((4, 0, False, 1, 3), (4, 0, True, 1, 3), (RZ, 7, False, 1, 3),
                                       (1, 7, False, 1, 3), (2, 7, False, 1, 3), (3, 7, False, 1, 3),
                                       (4, 7, False, RZ, 3), (4, 7, False, 1, RZ)):
                with self.subTest(rd=rd, pred=pred, inv=inv, a=a, b=b):
                    op = dict(name=f'guard_{rd}_{pred}_{inv}_{a}_{b}', arch='sm50', program=program('r', 0, rd=rd, pred=pred, inv=inv, a=a, b=b),
                              ins=['u', 'u', 'u'], outs=['u'], rows_in=inputs)
                    want = [[expected(0 if a == RZ else av, sv, 0 if b == RZ else bv, 0)
                             if rd != RZ and (pred == 7 or ((i % 32 == 0) != inv)) else 0xcdcdcdcd]
                            for i, (av, sv, bv) in enumerate(inputs)]
                    stage, got = run_op(op, work, work / 'run', steps, xc, dict(os.environ))
                    self.assertEqual(stage, 'ok', got)
                    self.assertEqual(got, want)


if __name__ == '__main__': unittest.main()
