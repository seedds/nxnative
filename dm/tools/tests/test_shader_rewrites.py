"""The builder's text rewrites found by text where a pattern tried every character of the source (dm/gfx/tools/dmsh_build.py, dmsh_fetch.py, dmsh_side.py, dmsh_atomic.py, GitHub
issue #1036): each gives the answer the pattern gave, at the edges of a word and of the source. No game files, no Metal compiler."""
import ast
import collections
from pathlib import Path
import random
import re
import struct
import sys
import tempfile
from typing import Any
import unittest

REPO = Path(__file__).resolve().parents[3]
TOOLS = REPO / 'dm/gfx/tools'
sys.path.insert(0, str(TOOLS))
import dmsh_atomic  # noqa: E402
import dmsh_fetch  # noqa: E402
import dmsh_side  # noqa: E402

ns: dict[str, Any] = {'re': re, 'collections': collections, 'dmsh_side': dmsh_side}   # dmsh_build.py is a script that runs a build when imported
exec(compile(ast.Module(body=[n for n in ast.parse((TOOLS / 'dmsh_build.py').read_text()).body if isinstance(n, ast.FunctionDef) and n.name in {'attributes', 'in_word', 'has_word', 'int_outputs', 'fs_uint'}], type_ignores=[]), 'dmsh_build', 'exec'), ns)
attributes, has_word, fs_uint = ns['attributes'], ns['has_word'], ns['fs_uint']


def noise(rng, parts, n=40):
    return ''.join(rng.choice(parts) for _ in range(n))


class AssignedOnce(unittest.TestCase):
    def test_a_name_assigned_once(self):
        self.assertTrue(dmsh_fetch._once('float _12 = 1.0;\n', '_12'))

    def test_a_name_assigned_twice(self):
        self.assertFalse(dmsh_fetch._once('float _12 = 1.0;\n_12 = 2.0;\n', '_12'))

    def test_a_longer_name_or_a_member_is_another_name(self):
        for src in ('a_12 = 1;', 'x._12 = 1;', '9_12 = 1;'):
            self.assertFalse(dmsh_fetch._once(src, '_12'), src)
        self.assertTrue(dmsh_fetch._once('x._12 = 1; _12 = 2;', '_12'))

    def test_at_the_start_of_the_source(self):
        self.assertTrue(dmsh_fetch._once('_12 = 1;', '_12'))

    def test_as_the_pattern_counts(self):
        rng = random.Random(1036)
        for _ in range(3000):
            src = noise(rng, ['_1', '_12', ' = ', ' ', '.', 'a', '\n', '9', ';'])
            self.assertEqual(dmsh_fetch._once(src, '_12'), len(re.findall(r'(?<![\w.])_12 = ', src)) == 1, src)


class HasWord(unittest.TestCase):
    def test_a_word_boundary_before_the_text(self):
        self.assertTrue(has_word('t.get_width(', 't.get_width('))
        self.assertTrue(has_word('a.t.get_width(', 't.get_width('))
        self.assertFalse(has_word('at.get_width(', 't.get_width('))
        self.assertFalse(has_word('_t.get_width(', 't.get_width('))

    def test_a_later_match_counts_after_an_earlier_refused_one(self):
        self.assertTrue(has_word('at.x( t.x(', 't.x('))

    def test_as_the_pattern_finds(self):
        rng = random.Random(1036)
        for _ in range(3000):
            src = noise(rng, ['t.w(', 'a', '_', ' ', '.', '\n', '('], 12)
            self.assertEqual(has_word(src, 't.w('), bool(re.search(r'\bt\.w\(', src)), src)


class Attributes(unittest.TestCase):
    OLD = r'(\w+)\s+(\w+)\s*\[\[attribute\((\d+)\)\]\]'

    def test_no_attribute(self):
        self.assertEqual(attributes('fragment float4 main0() {}\n'), [])

    def test_type_name_and_location(self):
        src = 'struct main0_in\n{\n    float4 m_0 [[attribute(0)]];\n    uint4 m_1 [[attribute(3)]];\n};\n\nvertex main0_out main0() {}\n'
        self.assertEqual([m.groups() for m in attributes(src)], [m.groups() for m in re.finditer(self.OLD, src)])
        self.assertEqual([m.groups() for m in attributes(src)], [('float4', 'm_0', '0'), ('uint4', 'm_1', '3')])

    def test_the_last_declaration_ends_the_source(self):
        self.assertEqual([m.group(3) for m in attributes('float4 m_0 [[attribute(7)]]')], ['7'])

    def test_on_the_first_line(self):
        self.assertEqual([m.group(2) for m in attributes('float4 m_0 [[attribute(7)]];\nint x;\n')], ['m_0'])


class Close(unittest.TestCase):
    def test_nested(self):
        src = '(a(b)c)d'
        self.assertEqual(dmsh_side._close(src, 1, '(', ')'), 7)

    def test_braces_only_count_braces(self):
        src = '{ ( } )'
        self.assertEqual(dmsh_side._close(src, 1, '{', '}'), 5)

    def test_unclosed(self):
        self.assertEqual(dmsh_side._close('(a(b)', 1, '(', ')'), -1)
        self.assertEqual(dmsh_side._close('(', 1, '(', ')'), -1)

    def test_as_the_character_loop(self):
        def loop(src, i, o, c):
            depth = 1
            while depth and i < len(src): depth += (src[i] == o) - (src[i] == c); i += 1
            return i if not depth else -1
        rng = random.Random(1036)
        for _ in range(3000):
            src = '(' + noise(rng, ['(', ')', 'a', '{', '}'], 20)
            self.assertEqual(dmsh_side._close(src, 1, '(', ')'), loop(src, 1, '(', ')'), src)


class IntegerOutputs(unittest.TestCase):
    def source(self, body, extra=''):
        return f'struct main0_out\n{{\n    float4 m_0 [[color(0)]];\n}};\n\n{extra}fragment main0_out main0(main0_in in [[stage_in]])\n{{\n    main0_out out = {{}};\n{body}\n    return out;\n}}\n'

    def test_an_output_from_integer_bits(self):
        self.assertIsNotNone(fs_uint(self.source('    out.m_0.x = as_type<float>(1u);'))[0])

    def test_a_computed_float_output_gets_none(self):
        self.assertIsNone(fs_uint(self.source('    out.m_0 = in.m_1 * 2.0;'))[0])

    def test_a_longer_name_is_not_the_output(self):
        self.assertIsNone(fs_uint(self.source('    xout.m_0.x = as_type<float>(1u);\n    out.m_0 = in.m_1 * 2.0;'))[0])

    def test_a_flat_input_passed_through(self):
        src = self.source('    out.m_0 = in.m_1;', 'struct main0_in\n{\n    float4 m_1 [[user(locn1), flat]];\n};\n\n')
        self.assertIsNotNone(fs_uint(src)[0])
        self.assertIsNone(fs_uint(src.replace(', flat]]', ']]'))[0])


class ImageAtomics(unittest.TestCase):
    MAGIC = [0x07230203, 0x10000, 0, 8, 0]
    NOP = [1 << 16]
    POINTER = [6 << 16 | 60, 1, 2, 3, 4, 5]

    def module(self, words):
        f = tempfile.NamedTemporaryFile(suffix='.spv', delete=False); f.write(struct.pack(f'<{len(words)}I', *words)); f.close()
        self.addCleanup(Path(f.name).unlink)
        return f.name

    def test_a_module_without_the_instruction(self):
        self.assertFalse(dmsh_atomic.uses_image_atomics(self.module(self.MAGIC + self.NOP)))

    def test_a_module_with_the_instruction(self):
        self.assertTrue(dmsh_atomic.uses_image_atomics(self.module(self.MAGIC + self.NOP + self.POINTER)))

    def test_the_word_as_an_operand_is_not_the_instruction(self):
        self.assertFalse(dmsh_atomic.uses_image_atomics(self.module(self.MAGIC + [2 << 16 | 2, 6 << 16 | 60])))

    def test_not_a_module(self):
        with self.assertRaises(ValueError): dmsh_atomic.uses_image_atomics(self.module([1, 2, 3, 4, 5, 6]))
        with self.assertRaises(ValueError): dmsh_atomic.uses_image_atomics(self.module([0x07230203]))

    def test_emulated_atomics_still_fail(self):
        for src in ('spvImage2DAtomicCoord(x)', 'device atomic_uint* t_atomic'):
            with self.assertRaises(ValueError): dmsh_atomic.check_msl(src)
        self.assertEqual(dmsh_atomic.check_msl('float4 x;'), 'float4 x;')


if __name__ == '__main__':
    unittest.main()
