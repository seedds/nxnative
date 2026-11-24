#!/usr/bin/env python3
"""else-if chains in the SPIR-V back end through the offline translator (dmsh_xlate, arch sm86).

Synthetic sm86 pixel programs compute their predicates before branching so that in normal mode (XLATE_DEBUG=0)
AstOptimizer.RemoveEmptyBlocks used to flatten an Else holding only an If into an ElseIf block:

    - three-way:  if (x > 1) r = 1; else if (x > 0) r = 2; else r = 3;
    - four-way:   if (x > 2) r = 1; else if (x > 1) r = 2; else if (x > 0) r = 3; else r = 4;
    - no-else:    r = 4; if (x > 1) r = 1; else if (x > 0) r = 2;

SPIR-V has no else-if construct, and SpirvGenerator only accepted Else as an If block's false target, so the false
edge went straight to the merge block and every ElseIf and trailing Else body became unreachable (still accepted by
spirv-val, then dropped by SPIRV-Cross). Keeping nested Else { If } blocks for SPIR-V preserves every branch cleanly.
Debug mode adds a comment node per instruction so no ElseIf formed there and serves as the control.
Needs DMSH_TEST_XLATE (the translator) like the other translator tests; no game data.
"""
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent / 'tools'
sys.path.insert(0, str(TOOLS))
import dmsh_chain  # noqa: E402

RZ, PT = 255, 7
CONTROL = 0x000fea0000000000 << 64   # stall 5, yield, no barriers


def inst(op, guard=PT, ginv=0, rd=RZ, ra=RZ, imm=None, fields=()):
    v = op | (guard << 12) | (ginv << 15) | (rd << 16) | (ra << 24)
    if imm is not None:
        v |= (imm & 0xffffffff) << 32
    for bit, width, val in fields:
        v |= (val & ((1 << width) - 1)) << bit
    return v | CONTROL


def f32(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def mov_imm(rd, value):
    return inst(0x802, rd=rd, imm=value, fields=((72, 4, 0xf),))


def mov_cbuf(rd, bank, offset):
    return inst(0xa02, rd=rd, fields=((38, 16, offset // 4), (54, 5, bank), (72, 4, 0xf)))


def isetp_gt(pd, ra, imm):   # ISETP.GT.AND Pd, PT, Ra, imm, PT (signed)
    return inst(0x80c, ra=ra, imm=imm, fields=((73, 1, 1), (76, 3, 4), (81, 3, pd), (84, 3, PT), (87, 3, PT)))


def bra(here, target, guard=PT, ginv=0):
    return inst(0x947, guard=guard, ginv=ginv, fields=((34, 48, (target - here - 16) // 4), (87, 3, PT)))


def pixel_program(code):
    """0x80-byte SPH header (pixel shader, render target 0 RGBA) and 128-bit Ampere instructions."""
    header = bytearray(0x80)
    struct.pack_into('<I', header, 0, 2 | (3 << 5) | (5 << 10))
    struct.pack_into('<I', header, 18 * 4, 0xf)
    return bytes(header) + b''.join(struct.pack('<QQ', c & ((1 << 64) - 1), c >> 64) for c in code)


def program_three_way():
    return pixel_program([
        mov_imm(1, f32(0.0)), mov_imm(2, f32(0.0)), mov_imm(3, f32(1.0)),
        mov_cbuf(4, 3, 0),                     # 0x30: R4 = c[3][0]
        isetp_gt(0, 4, 1),                     # 0x40: P0 = R4 > 1
        isetp_gt(1, 4, 0),                     # 0x50: P1 = R4 > 0
        bra(0x60, 0x90, guard=0, ginv=1),      # 0x60: @!P0 BRA 0x90
        mov_imm(0, f32(1.0)),                  # 0x70: R0 = 1.0
        bra(0x80, 0xd0),                       # 0x80: BRA 0xd0
        bra(0x90, 0xc0, guard=1),              # 0x90: @P1 BRA 0xc0 (in normal mode, the else holds only an If -> ElseIf)
        mov_imm(0, f32(3.0)),                  # 0xa0: R0 = 3.0
        bra(0xb0, 0xd0),                       # 0xb0: BRA 0xd0
        mov_imm(0, f32(2.0)),                  # 0xc0: R0 = 2.0
        inst(0x94d, fields=((87, 3, PT),)),    # 0xd0: EXIT
        bra(0xe0, 0xe0),
    ])


def program_four_way():
    return pixel_program([
        mov_imm(1, f32(0.0)), mov_imm(2, f32(0.0)), mov_imm(3, f32(1.0)),
        mov_cbuf(4, 3, 0),                     # 0x30: R4 = c[3][0]
        isetp_gt(0, 4, 2),                     # 0x40: P0 = R4 > 2
        isetp_gt(1, 4, 1),                     # 0x50: P1 = R4 > 1
        isetp_gt(2, 4, 0),                     # 0x60: P2 = R4 > 0
        bra(0x70, 0xa0, guard=0, ginv=1),      # 0x70: @!P0 BRA 0xa0
        mov_imm(0, f32(1.0)),                  # 0x80: R0 = 1.0
        bra(0x90, 0x110),                      # 0x90: BRA 0x110
        bra(0xa0, 0xd0, guard=1, ginv=1),      # 0xa0: @!P1 BRA 0xd0 (first ElseIf)
        mov_imm(0, f32(2.0)),                  # 0xb0: R0 = 2.0
        bra(0xc0, 0x110),                      # 0xc0: BRA 0x110
        bra(0xd0, 0x100, guard=2),             # 0xd0: @P2 BRA 0x100 (second ElseIf)
        mov_imm(0, f32(4.0)),                  # 0xe0: R0 = 4.0
        bra(0xf0, 0x110),                      # 0xf0: BRA 0x110
        mov_imm(0, f32(3.0)),                  # 0x100: R0 = 3.0
        inst(0x94d, fields=((87, 3, PT),)),    # 0x110: EXIT
        bra(0x120, 0x120),
    ])


def program_no_else():
    return pixel_program([
        mov_imm(0, f32(4.0)),                  # 0x00: R0 = 4.0
        mov_imm(1, f32(0.0)), mov_imm(2, f32(0.0)), mov_imm(3, f32(1.0)),
        mov_cbuf(4, 3, 0),                     # 0x40: R4 = c[3][0]
        isetp_gt(0, 4, 1),                     # 0x50: P0 = R4 > 1
        isetp_gt(1, 4, 0),                     # 0x60: P1 = R4 > 0
        bra(0x70, 0xa0, guard=0, ginv=1),      # 0x70: @!P0 BRA 0xa0
        mov_imm(0, f32(1.0)),                  # 0x80: R0 = 1.0
        bra(0x90, 0xc0),                       # 0x90: BRA 0xc0
        bra(0xa0, 0xc0, guard=1, ginv=1),      # 0xa0: @!P1 BRA 0xc0 (ElseIf with no trailing Else)
        mov_imm(0, f32(2.0)),                  # 0xb0: R0 = 2.0
        inst(0x94d, fields=((87, 3, PT),)),    # 0xc0: EXIT
        bra(0xd0, 0xd0),
    ])


CASES = (
    ('three_way', program_three_way, (1.0, 2.0, 3.0)),
    ('four_way', program_four_way, (1.0, 2.0, 3.0, 4.0)),
    ('no_else', program_no_else, (1.0, 2.0, 4.0)),
)


def unreachable_blocks(spv):
    """Labels that no branch, merge, switch or phi names, other than a function's first label."""
    words = struct.unpack(f'<{len(spv) // 4}I', spv)
    labels, refs, first = [], set(), True
    i = 5
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 0xffff
        operands = words[i + 1:i + count]
        if op == 54:                       # OpFunction
            first = True
        elif op == 248:                    # OpLabel
            if not first:
                labels.append(operands[0])
            first = False
        elif op == 249:                    # OpBranch
            refs.add(operands[0])
        elif op == 250:                    # OpBranchConditional
            refs.update(operands[1:3])
        elif op == 247:                    # OpSelectionMerge
            refs.add(operands[0])
        elif op == 246:                    # OpLoopMerge
            refs.update(operands[0:2])
        elif op == 251:                    # OpSwitch
            refs.add(operands[1])
            refs.update(operands[3::2])
        elif op == 245:                    # OpPhi
            refs.update(operands[3::2])
        i += count
    return [x for x in labels if x not in refs]


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class ElseIfChains(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.xlate = os.environ['DMSH_TEST_XLATE']
        spirv_cross = dmsh_chain.SPIRV_CROSS if Path(dmsh_chain.SPIRV_CROSS).is_file() else shutil.which('spirv-cross')
        if not spirv_cross:
            raise unittest.SkipTest('needs spirv-cross')
        cls.spirv_cross = spirv_cross
        cls.spirv_val = shutil.which('spirv-val')
        cls.tmp = tempfile.TemporaryDirectory(prefix='spirv_else_if_')
        cls.addClassCleanup(cls.tmp.cleanup)
        work = Path(cls.tmp.name)
        jobs = []
        cls.spv = {}
        for name, builder, _ in CASES:
            code_path = work / f'{name}.bin'
            code_path.write_bytes(builder())
            for mode, debug in (('normal', False), ('debug', True)):
                key = f'{name}_{mode}'
                jobs.append(dict(key=key, stage='ps', arch='sm86', code=str(code_path),
                                 tex={}, img={}, attr={}, fout={}, debug=debug))
                cls.spv[(name, mode)] = work / f'{key}.spv'
        jobs_path = work / 'jobs.json'
        jobs_path.write_text(json.dumps(jobs))
        run = subprocess.run([cls.xlate, str(jobs_path), str(work)], capture_output=True, text=True, timeout=120)
        if run.returncode != 0:
            raise AssertionError(f'dmsh_xlate failed:\n{run.stdout}\n{run.stderr}')

    def test_every_block_is_reachable(self):
        for (name, mode), path in self.spv.items():
            with self.subTest(case=name, mode=mode):
                self.assertTrue(path.is_file())
                self.assertEqual(unreachable_blocks(path.read_bytes()), [], f'{name} ({mode})')

    def test_modules_validate(self):
        if not self.spirv_val:
            self.skipTest('needs spirv-val')
        for (name, mode), path in self.spv.items():
            with self.subTest(case=name, mode=mode):
                r = subprocess.run([self.spirv_val, '--target-env', 'vulkan1.2', str(path)],
                                   capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_msl_keeps_every_branch_result(self):
        for name, _, expected in CASES:
            for mode in ('normal', 'debug'):
                path = self.spv[(name, mode)]
                with self.subTest(case=name, mode=mode):
                    msl = subprocess.run([self.spirv_cross, *dmsh_chain.CROSS_MSL, str(path)],
                                         capture_output=True, text=True, check=True).stdout
                    # The programs move each branch result as an integer bit pattern; SPIRV-Cross drops
                    # unreachable blocks, so a branch without a way in loses its constant from the MSL.
                    missing = [value for value in expected
                               if not any(tok in msl for tok in (str(f32(value)), hex(f32(value)), f'0x{f32(value):08X}'))]
                    self.assertEqual(missing, [], f'{name} ({mode}): missing {missing} from MSL:\n{msl}')


if __name__ == '__main__':
    unittest.main()
