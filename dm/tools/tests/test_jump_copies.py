"""Exercise the rewriter's copied-base and masked-predecessor jump table recognition on real AArch64 instructions, without game files."""
import ast
from pathlib import Path
import re
import struct
import sys
import unittest

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from capstone.arm64 import ARM64_OP_IMM

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from nso_asm import ADRP, ADDI, ADDR, B, BLR, BR, NOP

# patch_nso.py is a command-line program whose top level loads and rewrites an image.
# Compile its actual recognition functions without executing that driver.
tree = ast.parse((TOOLS / 'patch_nso.py').read_text())
IMG = bytearray(0x5000); tabjumps = {}; jumps = {}   # the globals the recognizers read: the module image and the jump tables resolved so far in the function
scope = {'re': re, 'struct': struct, 'ARM64_OP_IMM': ARM64_OP_IMM, 'IMG': IMG, 'tabjumps': tabjumps, 'jumps': jumps}
exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in ('num', 'copied_base', 'table_base', 'jump_masked_pred', 'check_before_copy')], type_ignores=[]), str(TOOLS / 'patch_nso.py'), 'exec'), scope)


def mov(dst, src, wide=True): return (0xaa0003e0 if wide else 0x2a0003e0) | src << 16 | dst


class CopiedBase(unittest.TestCase):
    def follow(self, words, reg='x7'):
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
        ins = list(md.disasm(struct.pack('<' + 'I' * len(words), *words), 0))
        self.assertEqual(len(ins), len(words))
        return scope['copied_base'](ins, len(ins), reg)

    def base(self, reg=7): return [ADRP(reg, 0, 0x4000), ADDI(reg, reg, 0x40c)]

    def test_far_copy_through_lr(self):
        self.assertEqual(self.follow(self.base() + [mov(30, 7)] + [NOP] * 688 + [mov(7, 30)]), 0x440c)

    def test_32_bit_writers_destroy_base(self):
        for clobber in (mov(7, 0, False), 0x52800027, ADDI(7, 7, 1) & 0x7fffffff):
            with self.subTest(clobber=hex(clobber)):
                self.assertIsNone(self.follow(self.base() + [clobber, mov(30, 7), mov(7, 30)]))
        self.assertIsNone(self.follow(self.base() + [mov(30, 7), mov(30, 0, False), mov(7, 30)]))

    def test_calls_clobber_volatile_and_link_registers(self):
        for call in (0x94000000, BLR(0)):
            self.assertIsNone(self.follow(self.base() + [call]))
            self.assertIsNone(self.follow(self.base() + [mov(30, 7), call, mov(7, 30)]))
            self.assertEqual(self.follow(self.base(19) + [call, mov(7, 19)]), 0x440c)

    def test_aliases_and_unrelated_writes(self):
        self.assertEqual(self.follow(self.base(29) + [mov(7, 29)]), 0x440c)
        self.assertEqual(self.follow(self.base(30), 'lr'), 0x440c)
        self.assertEqual(self.follow(self.base() + [mov(8, 0, False)]), 0x440c)
        self.assertIsNone(self.follow(self.base() + [mov(7, 0)]))


RET = 0xd65f03c0
AND_X6_X17_7FF = 0x92400000 | 10 << 10 | 17 << 5 | 6      # and x6, x17, #0x7ff
AND_X6_X17_FF = 0x92400000 | 7 << 10 | 17 << 5 | 6        # and x6, x17, #0xff
AND_W18_W17_7FF = 0x12000000 | 10 << 10 | 17 << 5 | 18    # and w18, w17, #0x7ff
UBFX_W18_W17 = 0x53000000 | 11 << 16 | 12 << 10 | 17 << 5 | 18   # ubfx w18, w17, #0xb, #2
CMP_W18_3 = 0x7100001f | 3 << 10 | 18 << 5                # cmp w18, #3
CMP_W18_1 = 0x7100001f | 1 << 10 | 18 << 5                # cmp w18, #1
LDRSW_X18 = 0xb8a07800 | 6 << 16 | 27 << 5 | 18           # ldrsw x18, [x27, x6, lsl #2]


def cbz_w18(frm, to): return 0x34000000 | (((to - frm) >> 2) & 0x7ffff) << 5 | 18
def bhi(frm, to): return 0x54000008 | (((to - frm) >> 2) & 0x7ffff) << 5


class MaskedPredecessor(unittest.TestCase):
    """The block `and w18,w17,#M; cmp w18,#3; b.hi; ldrsw x18,[x27,x6,lsl #2]; add; br x18` whose index x6 was masked from x17 in the block that branches to it.
    Word indices of the synthetic function: 0-1 base (adrp + add), 2-4 the predecessor (and x6, ubfx, cbz), 5 filler, 6 ret, 7-15 the dispatch block (and at 7, slots at 10, 12 and 14),
    16 ret, 17-20 the four targets. The table is at 0x4040 in the image."""
    TABLE = 0x4040
    TARGETS = [0x44, 0x48, 0x4c, 0x50]

    def setUp(self):
        for i, t in enumerate(self.TARGETS): struct.pack_into('<i', IMG, self.TABLE + 4 * i, t - self.TABLE)
        tabjumps.clear(); jumps.clear()

    def words(self, **over):
        w = [ADRP(27, 0, self.TABLE), ADDI(27, 27, self.TABLE & 0xfff), AND_X6_X17_7FF, UBFX_W18_W17, cbz_w18(16, 28), NOP, RET,
             AND_W18_W17_7FF, CMP_W18_3, bhi(36, 64), NOP, LDRSW_X18, NOP, ADDR(18, 18, 27), NOP, BR(18), RET, NOP, NOP, NOP, NOP]
        for k, v in over.items(): w[int(k[1:])] = v
        return w

    def run_site(self, words, undecodable=False):
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
        # The driver skips words Capstone cannot decode and resumes at the next word.
        ins = [p for i, word in enumerate(words) for p in md.disasm(struct.pack('<I', word), 4 * i)]
        self.assertEqual(len(ins), len(words) - int(undecodable))
        return scope['jump_masked_pred'](ins, next(i for i, p in enumerate(ins) if p.mnemonic == 'br' and p.op_str == 'x18'))

    def test_recognized(self):
        self.assertEqual(self.run_site(self.words()), self.TARGETS)

    def test_base_can_be_hoisted_far_back_or_built_in_dispatch(self):
        for local in (False, True):
            with self.subTest(local=local):
                words = self.words()
                if local:
                    words[:2] = [NOP, NOP]
                    words[10] = ADRP(27, 40, self.TABLE)
                    words.insert(11, ADDI(27, 27, self.TABLE & 0xfff))
                    words[16:16] = [mov(22, 30)] * 10   # ten register moves after the add
                    shift = 44; words[9] = bhi(36, 64 + shift)
                else:
                    words[2:2] = [NOP] * 300   # base more than 256 instructions before the load
                    shift = 1200
                targets = [t + shift for t in self.TARGETS]
                for i, t in enumerate(targets): struct.pack_into('<i', IMG, self.TABLE + 4 * i, t - self.TABLE)
                self.assertEqual(self.run_site(words), targets)

    def test_compare_in_the_block_does_not_count_as_a_write(self):
        self.assertEqual(self.run_site(self.words(w14=CMP_W18_1)), self.TARGETS)

    def test_predecessor_must_mask_the_same_way(self):
        self.assertIsNone(self.run_site(self.words(w2=AND_X6_X17_FF)))
        self.assertIsNone(self.run_site(self.words(w2=NOP)))
        self.assertIsNone(self.run_site(self.words(w3=0x2a0003f1)))   # mov w17, w0: the source changes between the and and the branch

    def test_block_entered_only_by_branches(self):
        self.assertIsNone(self.run_site(self.words(w6=NOP)))   # falls through into the and
        self.assertIsNone(self.run_site(self.words(w5=cbz_w18(20, 44))))   # another branch lands inside the block, past its and
        tabjumps[0x1000] = {0x20}   # a jump table already resolved in the function lands on the cmp
        self.assertIsNone(self.run_site(self.words()))
        tabjumps.clear(); jumps[0x1004] = (0x20, 4)   # a byte or half-word table that reaches the block
        self.assertIsNone(self.run_site(self.words()))

    def test_registers_unchanged_between_check_and_use(self):
        self.assertIsNone(self.run_site(self.words(w10=mov(6, 0))))   # the index changes after the check
        self.assertIsNone(self.run_site(self.words(w12=mov(27, 0))))   # the base changes between the load and the add
        self.assertIsNone(self.run_site(self.words(w12=mov(18, 0))))   # x18 is rewritten before the add
        self.assertIsNone(self.run_site(self.words(w14=mov(18, 0))))   # x18 is rewritten after the add

    def test_tables_cannot_bypass_predecessor_mask(self):
        for target in (12, 16):   # the ubfx or cbz, after the predecessor's and
            with self.subTest(target=target, table='offset'):
                tabjumps[0x1000] = {target}
                self.assertIsNone(self.run_site(self.words()))
                tabjumps.clear()
            with self.subTest(target=target, table='byte'):
                jumps[0x1004] = (target, 1)
                self.assertIsNone(self.run_site(self.words()))
                jumps.clear()

    def test_table_can_enter_at_predecessor_mask(self):
        tabjumps[0x1000] = {8}; jumps[0x1004] = (8, 1)
        self.assertEqual(self.run_site(self.words()), self.TARGETS)

    def test_calls_cannot_enter_after_a_mask(self):
        for target in (12, 16, 28, 44):   # predecessor, dispatch head or load
            with self.subTest(target=target):
                self.assertIsNone(self.run_site(self.words(w5=B(20, target) | 0x80000000)))

    def test_32_bit_writer_destroys_table_base(self):
        self.assertIsNone(self.run_site(self.words(w5=mov(27, 0, wide=False))))

    def test_undecodable_words_cannot_be_targets_or_hide_writes(self):
        for slot in (3, 10, 17):   # predecessor, dispatch, target
            with self.subTest(slot=slot):
                self.assertIsNone(self.run_site(self.words(**{f'w{slot}': 0xe7ffdefe}), undecodable=True))

    def test_targets_must_be_instructions_of_the_function(self):
        struct.pack_into('<i', IMG, self.TABLE + 12, 0x1000)
        self.assertIsNone(self.run_site(self.words()))

    def test_no_base_no_site(self):
        self.assertIsNone(self.run_site(self.words(w0=NOP, w1=NOP)))


class CheckBeforeCopy(unittest.TestCase):
    """#396: the bounds check may be separated from the load by the table base being built between
    them, and the index may be the checked register zero-extended after the check (Crash Bandicoot
    N. Sane Trilogy main+0x997b5c and main+0xefcc18). Each instruction word below was assembled
    with binutils and read back before being written here."""

    def bounds(self, words, idx):
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
        ins = list(md.disasm(struct.pack('<' + 'I' * len(words), *words), 0))
        self.assertEqual(len(ins), len(words))
        ld = next(p for p in ins if p.mnemonic == 'ldrsw')
        cmp = scope['check_before_copy'](ins, ins[:-1], ld, idx)
        m = re.fullmatch(r'(w\d+), #(0x[0-9a-f]+|\d+)', cmp.op_str) if cmp is not None else None
        return None if cmp is None or not m else (m.group(1), int(m.group(2), 0) + 1)

    def test_base_setup_between_check_and_load(self):
        # mov w18,w14; cmp w14,#3; b.hi; adrp x2; add x2,x2,#0x3a0; ldrsw x18,[x2,x18,lsl #2]; add x18,x18,x2; br x18
        ins = (0x2a0e03f2, 0x71000ddf, 0x540000c8, 0x90000002, 0x910e8042, 0xb8b27852, 0x8b020252, 0xd61f0240)
        self.assertEqual(self.bounds(ins, {'w18'}), ('w14', 4))

    def test_masked_index_after_the_check(self):
        # cmp w8,#3; b.hi; and x18,x8,#0xffffffff; ldrsw x18,[x17,x18,lsl #2]; add x18,x18,x17; br x18
        ins = (0x71000d1f, 0x540000a8, 0x92407d12, 0xb8b27a32, 0x8b110252, 0xd61f0240)
        self.assertEqual(self.bounds(ins, {'w18'}), ('w8', 4))

    def test_copy_far_before_the_check(self):
        # mov w0,w18; nop; nop; cmp w18,#4; b.hi; ldrsw x18,[x11,x0,lsl #2]; add x18,x18,x11; br x18 (DRAGON QUEST XI S main+0x2dd8cd0)
        ins = (0x2a1203e0, NOP, NOP, 0x7100125f, 0x54000088, 0xb8a07972, 0x8b0b0252, 0xd61f0240)
        self.assertEqual(self.bounds(ins, {'w0'}), ('w18', 5))

    def test_wrong_mask_rejects_the_site(self):
        # cmp w8,#3 (four entries needs every value 0..3 kept); and x18,x8,#2 keeps only 2 of them
        ins = (0x71000d1f, 0x540000a8, 0x927f0112, 0xb8b27a32, 0x8b110252, 0xd61f0240)
        self.assertIsNone(self.bounds(ins, {'w18'}))

    def test_index_written_between_copy_and_load(self):
        # mov w18,w5; cmp w5,#3; b.hi; add w18,w5,#1; ldrsw x18,[x2,x18,lsl #2]; add x18,x18,x2; br x18
        ins = (0x2a0503f2, 0x71000cbf, 0x540000a8, 0x110004b2, 0xb8b27852, 0x8b020252, 0xd61f0240)
        self.assertIsNone(self.bounds(ins, {'w18'}))


if __name__ == '__main__': unittest.main()
