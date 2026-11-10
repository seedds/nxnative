"""Real rewriter layout and instruction emission for sparse text longer than two B reaches."""
import ast
from itertools import combinations
from pathlib import Path
import re
import struct
import sys
from types import SimpleNamespace
import unittest

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import nso_asm

# The CLI loads a whole image at import. Execute its actual layout and emitters
# with a size-only image: these tests need the offsets, not hundreds of MB of padding.
TREE = ast.parse((TOOLS / 'patch_nso.py').read_text())
def assigns(node, name):
    return isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == name for t in node.targets)
FIRST = next(i for i, n in enumerate(TREE.body) if assigns(n, 'NEAR_PATCH_OFF'))
LAST = next(i for i, n in enumerate(TREE.body) if assigns(n, 'SPLIT'))
LAYOUT = TREE.body[FIRST:LAST + 1]
PLACEMENT = next(n for n in TREE.body if isinstance(n, ast.If) and isinstance(n.test, ast.Name) and n.test.id == 'FAR')
NAMES = {'use_area', 'here', 'emit', 'emit_key_addr', 'emit_slot_addr', 'dis', 'regset', 'num', 'regs_in', 'pick', 'subst', 'sp_delta', 'site_plain'}
EMITTERS = [n for n in TREE.body if isinstance(n, ast.FunctionDef) and n.name in NAMES]


def layout(text, size, bss=0, front=None):
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
    env = {} if front is None else {'DM_PATCH_FRONT': hex(front)}
    scope = vars(nso_asm).copy() | {'MOD': SimpleNamespace(image=range(size), text_end=text), 'IMG': range(size),
                                 'bss': bss, 'os': SimpleNamespace(environ=env), 'struct': struct, 're': re,
                                 'combinations': combinations, 'md': md, 'KEY_INDEX': {'K18': 0},
                                 'fix': [], 'patch_front': bytearray(), 'patch_back': bytearray()}
    exec(compile(ast.Module(body=LAYOUT + [PLACEMENT] + EMITTERS, type_ignores=[]), str(TOOLS / 'patch_nso.py'), 'exec'), scope)
    return scope


class PatchAreas(unittest.TestCase):
    def stub(self, scope, at):
        scope['use_area'](at)
        # mov x0,x18: run the actual scratch-register/TLS emitter, including its return B.
        ins = next(scope['md'].disasm(struct.pack('<I', 0xaa1203e0), at))
        start = scope['site_plain'](ins)
        entry = nso_asm.B(at, start)
        emitted = list(scope['md'].disasm(bytes(scope['patch']), scope['PATCH_BASE']))
        self.assertEqual(emitted[-1].mnemonic, 'b')
        self.assertEqual(emitted[-1].operands[0].imm, at + 4)
        self.assertEqual(next(scope['md'].disasm(struct.pack('<I', entry), at)).operands[0].imm, start)
        return start, bytes(scope['patch'])

    def test_sparse_large_text_uses_both_ends(self):
        # The #1504 shape: hundreds of MB of embedded data between small code islands.
        s = layout(0x1571f130, 0x15a20000, 0xe71fd8)
        self.assertEqual((s['PATCH_OFF'], s['BACK_OFF'], s['SPLIT']), (-0x1000000, 0x168a0000, 0xe9a0000))
        self.assertLess(self.stub(s, 0x100)[0], 0)
        self.assertGreaterEqual(self.stub(s, 0x1550407c)[0], s['BACK_OFF'])
        self.assertTrue(s['patch_front'] and s['patch_back'])

    def test_actual_site_in_unreachable_middle_is_still_rejected(self):
        s = layout(0x1571f130, 0x15a20000, 0xe71fd8)
        with self.assertRaises(AssertionError): self.stub(s, 0xb000000)

    def test_explicit_front_reservation_keeps_both_ends_reachable(self):
        s = layout(0x1571f130, 0x15a20000, 0xe71fd8, front=0x80000)
        self.assertEqual(s['PATCH_OFF'], -0x80000)
        self.stub(s, 0x100); self.stub(s, 0x1550407c)

    def test_existing_overlapping_layouts_and_emitted_stubs_are_unchanged(self):
        # FC27 and Hello Kitty's published BACK_OFF values; compact front reservation for the latter.
        for text, size, expected in [(0xb990000, 0xdeb0000, (0x1000000, 0x6f00000, 0xdec0000)),
                                     (0x97d0000, 0xfa30000, (0x3c0000, 0x7b40000, 0xfa40000))]:
            with self.subTest(size=hex(size)):
                s = layout(text, size)
                self.assertEqual((-s['PATCH_OFF'], s['SPLIT'], s['BACK_OFF']), expected)
                self.stub(s, s['SPLIT'] - 4); self.stub(s, s['SPLIT'])
                forced = layout(text, size, front=expected[0])
                self.stub(forced, forced['SPLIT'] - 4); self.stub(forced, forced['SPLIT'])
                self.assertEqual(s['patch_front'], forced['patch_front'])
                self.assertEqual(s['patch_back'], forced['patch_back'])

    def test_no_rewrite_sites_needs_no_artificial_contiguous_reach(self):
        s = layout(0x18000000, 0x18000000)
        self.assertEqual((s['patch_front'], s['patch_back']), (b'', b''))
        self.assertGreater(s['SPLIT'], 0x8000000)


if __name__ == '__main__': unittest.main()
