#!/usr/bin/env python3
"""Control flow through the offline translator (dmsh_xlate, arch sm86): forward jumps must not come out as loops, and a jump table must stop where the next one starts.
A loop the program does not have costs Metal's compilers dearly: Splatoon Raiders' uber shaders, with neither fix, took hours in the front end and crashed the GPU compiler.
Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import json, os, re, struct, subprocess, sys, tempfile, unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
SIZE = 16   # an instruction's bytes
PT = 7      # the predicate that is always true
EXIT = (0x000fea0003800000, 0x000000000000794d)   # hi, lo
# SUATOM.ADD.U32 R12, [R3], R12: a statement that cannot be optimized away (test_dmsh_ampere_mem.py), and that says which block it is in by nothing but its place
WORK = (0x002ea200001ee000, 0x00000c0c030c7593)


def bits(*fields):
    v = 0
    for lo, width, value in fields:
        v |= (value & ((1 << width) - 1)) << lo
    return (v >> 64) & (2 ** 64 - 1), v & (2 ** 64 - 1)


def branch(opcode, offset, pred=PT, ra=255):   # offset: bytes from the next instruction. Bits 87..90: BRA's condition predicate, always true here
    return bits((0, 12, opcode), (12, 3, pred), (24, 8, ra), (34, 48, offset // 4), (87, 3, PT))


def bra(at, to, pred=PT):
    return branch(0x947, to * SIZE - (at + 1) * SIZE, pred)


def ldc(rd, ra, offset):   # LDC Rd, c1[Ra + offset], 32 bits
    return bits((0, 12, 0xb82), (12, 3, PT), (16, 8, rd), (24, 8, ra), (38, 16, offset), (54, 5, 1), (73, 3, 4))


def assemble(insts):
    return b''.join(struct.pack('<QQ', lo, hi) for hi, lo in insts)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class ControlFlow(unittest.TestCase):
    def translate(self, insts, c1=b''):
        with tempfile.TemporaryDirectory(prefix='control_flow_') as d:
            work = Path(d); (work / 'k.bin').write_bytes(assemble(insts))
            job = [dict(key='k', stage='cs', arch='sm86', code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={}, local=[32, 1, 1], debug=True, nvn2_cs=[0, 0, 3, 0x40, 0], c1=c1.hex())]
            (work / 'jobs.json').write_text(json.dumps(job))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), d], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue((work / 'k.spv').exists(), r.stdout + r.stderr)
            sys.path.insert(0, str(TOOLS)); import dmsh_geometry
            dmsh_geometry.flatten_sets(work / 'k.spv')
            r = subprocess.run([str(TOOLS / 'spirv_cross_bin/spirv-cross'), '--msl', '--msl-version', '20300', '--msl-argument-buffers', str(work / 'k.spv')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            return r.stdout

    def assert_no_loop(self, msl):
        self.assertNotRegex(msl[msl.index('kernel void main0'):], r'\bdo\b|\bwhile\b|\bfor\b')

    def test_jump_into_what_follows_the_if(self):
        # if (!P0) { if (P1) goto M; work; goto N; }  L: work;  M: work;  N: exit. The goto to N moves out of the if, and an else around L and M would leave the goto to M only a loop
        program = [bra(0, 5, pred=0), bra(1, 6, pred=1), WORK, WORK, bra(4, 8), WORK, WORK, WORK, EXIT]
        msl = self.translate(program)
        self.assertEqual(msl.count('atomic_fetch_add'), 5, msl)   # every statement is still there
        self.assert_no_loop(msl)

    def test_switches_one_after_another(self):
        # The same jump out of each case, again and again: each else took the rest of the program, and each loop closed at its end
        program, at, n = [], 0, 6
        for _ in range(n):
            program += [bra(at, at + 5, pred=0), bra(at + 1, at + 6, pred=1), WORK, bra(at + 3, at + 7), WORK, WORK, WORK]
            at += 7
        program.append(EXIT)
        self.assert_no_loop(self.translate(program))

    def test_real_loop_stays(self):
        msl = self.translate([WORK, bra(1, 0, pred=0), EXIT])
        self.assertRegex(msl[msl.index('kernel void main0'):], r'\bwhile\b|\bfor\b')

    def two_switches(self, second_extra=()):
        # 0: LDC R1, c1[R2 + 0]; 1: BRX R1 to 2 or 3 (each goes on to the join at 4); 4: LDC R1, c1[R2 + 8]; 5: BRX R1 to 6 or 7; 8: exit.
        # The game's table words are offsets from one anchor, the instruction after the first BRX, and each BRX's immediate leads there, so one switch can read another's words as targets of its own
        anchor = 2 * SIZE
        program = [ldc(1, 2, 0), branch(0x949, 0, ra=1), bra(2, 4), WORK, ldc(1, 2, 8), branch(0x949, anchor - 6 * SIZE, ra=1), bra(6, 8), WORK, EXIT]
        words = [t * SIZE - anchor for t in (2, 3, 6, 7, *second_extra)]
        return program, struct.pack(f'<{len(words)}i', *words)

    def test_table_ends_at_the_next_table(self):
        # The second BRX is reached only through the first one's targets. Read at once, the first table takes the second's two targets as its own, so it compares with three addresses, not one
        msl = self.translate(*self.two_switches())
        self.assertEqual(len(re.findall(r'== (?:\w+\()?\d+', msl)), 2, msl)

    def test_table_loaded_before_a_join(self):
        # 0: LDC R1, c1[R2 + 0]; 1, 2: two branches to 4; 3: work; 4: BRX R1 to 5 or 6; 5: jump to 7; 6: work; 7: exit. The register is loaded before the join the BRX comes after
        anchor = 5 * SIZE
        program = [ldc(1, 2, 0), bra(1, 4, pred=0), bra(2, 4, pred=1), WORK, branch(0x949, 0, ra=1), bra(5, 7), WORK, EXIT]
        words = [t * SIZE - anchor for t in (5, 6)]
        msl = self.translate(program, struct.pack('<2i', *words))
        self.assertEqual(len(re.findall(r'== (?:\w+\()?\d+', msl)), 1, msl)

    def test_table_loaded_in_a_block_split_later(self):
        # 0: work; 1: LDC R1, c1[R2 + 0]; 2: if (P0) jump to 5; 3: if (P1) jump back to 1; 4: exit; 5: BRX R1 to 6 or 7; 6: jump to 8; 7: work; 8: exit.
        # Block 0..2 is decoded whole before the jump back splits it at 1, and the split left the BRX's predecessor naming 0..0, which has no LDC: "BRX with no
        # target found in the c1 jump table" (16 pixel shaders of Pokémon Pokopia)
        anchor = 6 * SIZE
        program = [WORK, ldc(1, 2, 0), bra(2, 5, pred=0), bra(3, 1, pred=1), EXIT, branch(0x949, 0, ra=1), bra(6, 8), WORK, EXIT]
        words = [t * SIZE - anchor for t in (6, 7)]
        msl = self.translate(program, struct.pack('<2i', *words))
        self.assertEqual(len(re.findall(r'== (?:\w+\()?\d+', msl)), 1, msl)

    def test_word_past_the_code_ends_the_table(self):
        # What follows a table in c1 is data, and may lead past the code's end: the translator took that as a failure (ArgumentOutOfRangeException, two compute shaders of Yoshi and the Mysterious Book)
        anchor = 2 * SIZE
        program = [ldc(1, 2, 0), branch(0x949, 0, ra=1), bra(2, 4), WORK, EXIT]
        words = [t * SIZE - anchor for t in (2, 3)] + [0x100000]
        msl = self.translate(program, struct.pack('<3i', *words))
        self.assertEqual(len(re.findall(r'== (?:\w+\()?\d+', msl)), 1, msl)

    def test_entry_for_program_start_is_no_target(self):
        # Words that lead to address 0 fill a table's unused cases: taken as targets, they put the whole program in a loop
        msl = self.translate(*self.two_switches(second_extra=(0, 0, 0)))
        self.assert_no_loop(msl)
        self.assertEqual(len(re.findall(r'== (?:\w+\()?\d+', msl)), 2, msl)


    def loop_exit(self, target):
        # A warp-aggregated atomic, as PRAGMATA's 607ceeefb9478700 does it with SURED.MIN in BSSY B3: in a loop, the lanes whose key matches the leader's branch out
        # (@!P0 BRA) to a block that reduces with subgroup operations, as one cohort, then join the others. GotoElimination.MoveOutward made it `if (f) break;` and ran
        # the block after the loop as `if (false || f)`, where the lanes of every iteration vote together: the ballot mixed keys and only one leader's atomic landed
        # (found by an offline differential check against a SASS reference model). LoopExitCohort moves the block into the loop, before the break.
        # 0: S2R R13, SR_TID.X; 1: VOTEU.ALL UR4 (the loop); 2: ISETP.NE.U32 P0, R13, RZ; 3: @!P0 BRA 9; 4: IADD3 R12 += 1; 5: ISETP.GE P0, R12, 0x100; 6: @!P0 BRA 1;
        # 7: work; 8: BRA past the target; 9..: the target (two instructions in the first test); then work (the join) and exit
        def gbra(at, to, pred=PT):   # the game's encoding: Ra = R0, bit 15 negates the predicate
            hi, lo = bits((0, 12, 0x947), (12, 4, pred), (34, 48, (to - at - 1) * SIZE // 4), (87, 3, PT))
            return hi | 0x000fea0000000000, lo
        wait = lambda i: (i[0] | 0x3f << 52, i[1])   # waits on every scoreboard: reads what the S2R wrote
        s2r = (0x000e260000002100, 0x00000000000d7919)
        voteu = (0x000fe400038e0000, 0x0000000000047886)   # VOTEU.ALL UR4, UPT, PT: the game's +0x0bc0 (and +0x0cc0, after the reduction's CALL)
        iadd = (0x000fc80007ffe0ff, 0x000000010c0c7810)    # IADD3 R12, R12, 0x1, RZ (the game's +0x0c20)
        program = [s2r, voteu, wait((0x001fda0003f05070, 0x000000ff0d00720c)), gbra(3, 9, pred=8 | 0), iadd,
                   (0x000fda0003f06270, 0x000001000c00780c), gbra(6, 1, pred=8 | 0), WORK, gbra(8, 9 + len(target)), *target, WORK, EXIT]
        return self.translate(program)

    def test_loop_exit_target_with_subgroup_ops_stays_in_the_iteration(self):
        msl = self.loop_exit(((0x000fe400038e0000, 0x0000000000047886), (0x000fe20008000f00, 0x00000004000c7c02)))   # VOTEU.ALL UR4; MOV R12, UR4
        main = msl[msl.index('kernel void main0'):]
        self.assertNotRegex(main, r'if \(false \|\| _\d+\)', main)
        loop = main[re.search(r'\bdo\b', main).start():]
        loop = loop[:loop.index('while')]
        # the target's ballot runs in the loop, under the exit's flag, right before the break
        self.assertRegex(loop, r'if \((_\d+)\)\s*\{[^{}]*spvSubgroupBallot[^{}]*break;\s*\}', main)
        self.assertEqual(msl.count('atomic_fetch_add'), 2, msl)   # the skipped work and the join, once each

    def test_loop_exit_target_that_exits_has_no_break_after_it(self):
        # A target that ends the program (EXIT): moved into the loop it must not be followed by the break, or the SPIR-V has a branch outside any block and SPIRV-Cross
        # stops ("Trying to end a non-existing block"; Sonic Frontiers' compute shader 0e6f8156033ae07e)
        msl = self.loop_exit(((0x000fe400038e0000, 0x0000000000047886), (0x000fe20008000f00, 0x00000004000c7c02), WORK, EXIT))   # VOTEU.ALL UR4; MOV R12, UR4; work; EXIT
        main = msl[msl.index('kernel void main0'):]
        self.assertNotRegex(main, r'if \(false \|\| _\d+\)', main)
        loop = main[re.search(r'\bdo\b', main).start():]
        loop = loop[:loop.index('while')]
        self.assertIn('spvSubgroupBallot', loop, main)   # the target, ballot and return, runs in the loop
        self.assertRegex(loop, r'return;\s*\}', main)
        self.assertNotRegex(loop, r'return;\s*break;', main)

    def test_loop_exit_target_without_subgroup_ops_stays_after_the_loop(self):
        # The move is only for targets with subgroup operations: other code keeps GotoElimination's form
        iadd = (0x000fc80007ffe0ff, 0x000000010c0c7810)
        self.assertRegex(self.loop_exit((iadd, iadd)), r'if \(false \|\| _\d+\)')


if __name__ == '__main__':
    unittest.main()
