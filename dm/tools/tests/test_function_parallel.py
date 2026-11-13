"""Independent rewriter analysis uses real Capstone instructions and fork workers, retaining cross-function guards."""
import ast
import multiprocessing
from pathlib import Path
import pickle
import re
import signal
import struct
import sys
import types
import unittest
from unittest import mock

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_MEM, ARM64_OP_REG

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import nso_asm
import rewrite_jobs


class FunctionAnalysis(unittest.TestCase):
    def setUp(self):
        tree = ast.parse((TOOLS / 'patch_nso.py').read_text())
        self.scope = types.ModuleType('rewrite_analysis_fixture'); sys.modules[self.scope.__name__] = self.scope
        self.addCleanup(sys.modules.pop, self.scope.__name__, None)
        d = self.scope.__dict__
        d.update({k: v for k, v in vars(nso_asm).items() if not k.startswith('__')})
        d.update(re=re, struct=struct, multiprocessing=multiprocessing, rewrite_jobs=rewrite_jobs, WORKERS=2, BIG_FN=0x40000, IMG=bytearray(0x21000),
                 signal=signal, jumps={}, tabjumps={}, ARM64_OP_IMM=ARM64_OP_IMM, ARM64_OP_MEM=ARM64_OP_MEM, ARM64_OP_REG=ARM64_OP_REG)
        d['md'] = Cs(CS_ARCH_ARM64, CS_MODE_ARM); d['md'].detail = True
        defs: list[ast.stmt] = [n for n in tree.body if isinstance(n, ast.FunctionDef) or isinstance(n, ast.Assign)
                and any(getattr(t, 'id', None) in ('EXCL_LD', 'EXCL_ST') for t in n.targets)]
        exec(compile(ast.Module(body=defs, type_ignores=[]), str(TOOLS / 'patch_nso.py'), 'exec'), d)
        self.assertTrue(hasattr(self.scope, 'analyze_functions'), 'function analysis has no parallel driver')

    def words(self, start, words):
        struct.pack_into('<' + 'I' * len(words), self.scope.IMG, start, *words)

    def test_real_function_analysis_overlaps_and_results_are_serializable(self):
        fn_size = 0x10000
        for start in (0, fn_size):
            self.words(start, [0x885f7d32, 0x88007d32, 0x35ffffc0, 0xd65f03c0] + [nso_asm.NOP] * (fn_size // 4 - 4))
        real = self.scope.disasm_fn; barrier = multiprocessing.get_context('fork').Barrier(2)
        def disasm(a, b):
            barrier.wait(timeout=5)
            return real(a, b)
        with mock.patch.object(self.scope, 'disasm_fn', disasm):
            result = list(self.scope.analyze_functions([(0, fn_size), (fn_size, 2 * fn_size)]))
        self.assertEqual(pickle.loads(pickle.dumps(result)), result)
        self.assertEqual(result[0][0], [[0, 4, 8]])
        self.assertEqual(result[1][0], [[fn_size, fn_size + 4, fn_size + 8]])
        self.assertEqual(self.scope.tabjumps, {})

    def test_retry_windows_have_the_same_result_with_one_or_multiple_workers(self):
        words = [0x885f7d52, 0x6b1201ff, 0x54000061, 0x88117d52, 0x35000051, 0xd65f03c0,
                 0x885f7d51, 0x6b1101ff, 0x54ffff60, 0xd65f03c0]
        size = 0x10000
        padded = words + [nso_asm.NOP] * (size // 4 - len(words))
        self.words(0, padded); self.words(size, padded)
        funcs = [(0, size), (size, 2 * size)]
        vars(self.scope)['WORKERS'] = 1; serial = list(self.scope.analyze_functions(funcs))
        vars(self.scope)['WORKERS'] = 2; parallel = list(self.scope.analyze_functions(funcs))
        self.assertEqual(serial, parallel)
        self.assertEqual(serial[0][0], [[0, 4, 8, 12, 16]])
        self.assertEqual(serial[0][5], [[([24, 28, 32], 36), ([12, 16], 20)]])
        self.assertEqual(serial[0][6], {28, 32})

    def test_a_previous_functions_jump_table_still_blocks_an_unsafe_proof(self):
        # The earlier byte table covers the later function's protected masked-index dispatch.
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from test_jump_copies import MaskedPredecessor
        self.words(0, [0x10000812, 0x38706a20, 0x8b000a52, nso_asm.BR(18), 0xd65f03c0])
        words = MaskedPredecessor().words()
        self.words(0x100, words + [nso_asm.NOP] * (0x10000 // 4 - len(words)))
        for i, target in enumerate([0x144, 0x148, 0x14c, 0x150]): struct.pack_into('<i', self.scope.IMG, 0x4040 + i * 4, target - 0x4040)
        self.assertEqual(self.scope.analyze_function(0x100, 0x100 + len(words) * 4)[3][0x13c], [0x144, 0x148, 0x14c, 0x150])
        for workers in (1, 2):
            with self.subTest(workers=workers):
                vars(self.scope)['WORKERS'] = workers
                with self.assertRaisesRegex(AssertionError, 'jump table'): list(self.scope.analyze_functions([(0, 20), (0x100, 0x10100)]))


if __name__ == '__main__': unittest.main()
