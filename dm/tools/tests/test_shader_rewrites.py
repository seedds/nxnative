"""The builder's text rewrites found by text where a pattern tried every character of the source (dm/gfx/tools/dmsh_build.py, dmsh_fetch.py, dmsh_side.py, dmsh_atomic.py, GitHub
issue #1036): each gives the answer the pattern gave, at the edges of a word and of the source. No game files, no Metal compiler."""
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
import dmsh_abi  # noqa: E402
import dmsh_atomic  # noqa: E402
import dmsh_entry  # noqa: E402
import dmsh_fetch  # noqa: E402
import dmsh_reflect  # noqa: E402
import dmsh_rewrite  # noqa: E402
import dmsh_side  # noqa: E402

ns: dict[str, Any] = {**vars(dmsh_reflect), **vars(dmsh_entry)}
attributes, has_word, fs_uint = ns['attributes'], ns['has_word'], ns['fs_uint']


def noise(rng, parts, n=40):
    return ''.join(rng.choice(parts) for _ in range(n))


class UniformMatchScan(unittest.TestCase):
    # The original assertion-first expression: keep an independent oracle for match order, spans and every capture.
    READ = r'as_type<(u?int)>\((\(\*spvDescriptorSet0\.[a-z]+_c(?:[3-9]|\d\d)\)\._m0\[\d+\])\.([xyzw])\)'
    OLD = re.compile(r'(?<![\w.])%s(?= [!=]= -?\d+u?(?![\w.]))|(?<=[!=]= )%s' %
                     (READ, READ.replace('(u?int)', '(?P<t2>u?int)').replace(r'(\(\*', r'(?P<r2>\(\*').replace('([xyzw])', '(?P<c2>[xyzw])')))

    def compare(self, src):
        old = [(m.span(), m.groups(), m.groupdict()) for m in self.OLD.finditer(src)]
        new = [(m.span(), m.groups(), m.groupdict()) for m in dmsh_rewrite._UTEST.finditer(src)]
        self.assertEqual(new, old, src)
        return len(old)

    def test_boundaries_and_both_comparison_orders(self):
        word = 'as_type<int>((*spvDescriptorSet0.fp_c5)._m0[16].w)'
        for prefix in ('', 'x', '_', '.', '\u00e9', '\u0301', '\x00', '1 == ', '-2u != ', ' = ', '<= ', '>= '):   # the last three: an = that is no [!=]=
            for suffix in ('', ' != 1', ' == -2u', ' == 2ux', ' >= 3', ' == _4'):
                self.compare(prefix + word + suffix)
        self.assertEqual(self.compare(word + ' != 1'), 1)
        self.assertEqual(self.compare('1 == ' + word + ' != 2'), 1)
        self.assertEqual(self.compare('1 == ' + word + '; ' + word + ' != 2'), 2)

    def test_as_the_original_pattern_on_random_text(self):
        rng = random.Random(1932)
        word = 'as_type<int>((*spvDescriptorSet0.fp_c5)._m0[16].w)'
        parts = [word, word.replace('<int>', '<uint>'), word.replace('c5', 'c0'), word.replace('c5', 'c123'), word.replace('16', '\u0661'),
                 ' != 1', ' == -2u', ' == _4', ' >= 3', '1 == ', '-2u != ', '12345678901234567890 == ', ' = ', '<= ', '>= ', 'x', '_', '.', '\u00e9', '\u0301', '\x00',
                 'as_type<', 'as_type<as_type<', ';\n', ' ', '1 == ' + word + ' != 2']
        matches = sum(self.compare(noise(rng, parts, rng.randrange(25))) for _ in range(10000))
        self.assertGreater(matches, 1000)   # the corpus must exercise matching paths, not only no-ops


class GuardScans(unittest.TestCase):
    # dmsh_rewrite.select_guards' scans against the patterns tried at every character, kept here as they were: the same matches in the same order, with their spans and groups
    SIZE = r'\(\(\*spvDescriptorSet0\.[a-z]+_c%d\)\._m0\[\d\]\.[xyzw] >> (?:2|4)u\)' % dmsh_abi.CBSIZE_BANK
    TABLE = r'\(\*spvDescriptorSet0\.[a-z]+_c%d\)\._m0\[\d+\]\.[xyzw]' % dmsh_abi.SBLEN_BANK
    GBOOL = re.compile(r'\bbool (_\d+) = [^;\n]* < %s;' % SIZE)
    SBOOL = re.compile(r'\bbool (_\d+) = \(uint\(_\d+\) \+ %s\) < %s;' % (TABLE, TABLE))
    NAME = re.compile(r'(?<!\[)\b(_\d+) \?')
    ROW, BUF = '(*spvDescriptorSet0.fp_c14)._m0[_5 ? _3 : 0]', '(*spvDescriptorSet0.cp_s3)._m0[_12 ? _3 : 0]'
    SIZED = 'bool _5 = _3 < ((*spvDescriptorSet0.fp_c%d)._m0[2].w >> 2u);' % dmsh_abi.CBSIZE_BANK
    ROW_SIZED = SIZED.replace(' >> 2u', ' >> 4u')
    LENGTH = 'bool _12 = (uint(_3) + {0}.z) < {0}.w;'.format('(*spvDescriptorSet0.cp_c%d)._m0[1]' % dmsh_abi.SBLEN_BANK)
    CHOICE, STORED = '_5 ? %s[_4] : 0.0' % ROW, '_12 ? %s : 0u' % BUF

    @staticmethod
    def seen(m):
        return m.span(), m.groups(), m.groupdict()

    def compare(self, src, conds=('_5', '_12')):
        """→ how many matches the two comparisons and the two choices have on src, and whether a name of conds is left: each scan as its pattern"""
        counts = []
        for new, old in ((dmsh_rewrite._GBOOL, self.GBOOL), (dmsh_rewrite._SBOOL, self.SBOOL)):
            want = [self.seen(m) for m in old.finditer(src)]
            self.assertEqual([self.seen(m) for m in new.finditer(src)], want, src); counts.append(len(want))
        def logged(log):   # a replacement that records its matches; every third is left as it is, as a choice on another name is
            def repl(m):
                log.append(self.seen(m)); return m.group(0) if len(log) % 3 == 0 else '<%d>' % len(log)
            return repl
        for pat in (dmsh_rewrite._GROW, dmsh_rewrite._SROW):
            want, got = [], []
            text = pat.sub(logged(want), src)
            self.assertEqual(dmsh_rewrite._gsub(pat, logged(got), src), text, src); self.assertEqual(got, want, src); counts.append(len(want))
        want = next((m for m in self.NAME.finditer(src) if m.group(1) in conds), None); got = dmsh_rewrite._gleft(src, conds)
        self.assertEqual(got and self.seen(got), want and self.seen(want), src)
        return counts + [int(bool(want))]

    def test_boundaries_and_both_forms(self):
        for prefix in ('', 'x', '_', '.', 'é', '́', '\x00', ' ', '(', '>', '>(', 'x(', '((', '[', '_5 ? '):
            for body in (self.SIZED, self.ROW_SIZED, self.LENGTH, self.CHOICE, '(%s)' % self.CHOICE, self.CHOICE.replace('[_4]', '.z'), self.STORED, '(%s)' % self.STORED, '_5 ? _7 : 0.0'):
                for suffix in ('', ';', ')', '.x', 'u', '1', ' ? ', dmsh_rewrite._GREAD, '\n'):
                    self.compare(prefix + body + suffix)
        self.assertEqual(self.compare(self.SIZED + '\nfloat _6 = %s;' % self.CHOICE), [1, 0, 1, 0, 1])
        self.assertEqual(self.compare(self.ROW_SIZED + '\nfloat _6 = %s;' % self.CHOICE), [1, 0, 1, 0, 1])
        self.assertEqual(self.compare(self.LENGTH + '\nuint _6 = as_type<uint>(%s) + (%s);' % (self.STORED, self.STORED)), [0, 1, 0, 2, 1])
        self.assertEqual(self.compare('xbool' + self.SIZED[4:] + ' float _6 = _7 * (%s);' % self.CHOICE), [0, 0, 1, 0, 1])
        self.assertEqual(self.compare('float _6 = %s[_4];' % self.ROW), [0, 0, 0, 0, 0])   # the index's own choice is no name's

    def test_as_the_patterns_on_random_text(self):
        rng = random.Random(1932)
        parts = [self.SIZED, self.ROW_SIZED, self.LENGTH, self.SIZED.replace('_5', '_١'), 'bool ', 'bool _7 = _3', ' < ', self.CHOICE, '(%s)' % self.CHOICE, self.CHOICE.replace('[_4]', '.z'),
                 self.CHOICE.replace('_5', '_7'), self.STORED, '(%s)' % self.STORED, self.ROW, self.BUF, '_5', '_12', '_7', '_5 ? ', '_12 ?', ' ? ', ' ?', ' : 0.0', ' : 0', 'u',
                 dmsh_rewrite._GREAD, '(*spvDescriptorSet0.fp_c14)._m0[', '(', ')', '[', ']', '>', '.', '_', 'x', '1', '١', 'é', '́', '\x00', ' ', ';', '\n', ' = ']
        totals = [0] * 5
        for _ in range(10000):
            totals = [a + b for a, b in zip(totals, self.compare(noise(rng, parts, rng.randrange(30))))]
        for n in totals: self.assertGreater(n, 1000)   # the corpus must exercise every scan's matching paths, not only no-ops


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


class TypedOutputs(unittest.TestCase):   # dmsh_entry.fs_uint: main0_u for every pixel shader of a known shape, whatever its code writes (GitHub issue #2760)
    def source(self, body, extra=''):
        return f'struct main0_out\n{{\n    float4 m_0 [[color(0)]];\n}};\n\n{extra}fragment main0_out main0(main0_in in [[stage_in]])\n{{\n    main0_out out = {{}};\n{body}\n    return out;\n}}\n'

    def test_every_shader_whatever_it_writes(self):   # the guesses it replaces: integer bits, a computed float, a flat input, a local, zeros, a uniform word
        flat = 'struct main0_in\n{\n    float4 m_1 [[user(locn1), flat]];\n};\n\n'
        for body, extra in [('    out.m_0.x = as_type<float>(1u);', ''), ('    out.m_0 = in.m_1 * 2.0;', ''), ('    out.m_0 = in.m_1;', flat), ('    out.m_0 = in.m_1;', flat.replace(', flat', '')),
                            ('    float _439 = as_type<float>(int(gl_PrimitiveID));\n    out.m_0.x = _439;', ''), ('    out.m_0.x = 0.0;', ''), ('    out.m_0.x = (*spvDescriptorSet0.fp_c8)._m0[2].w;', '')]:
            with self.subTest(body=body): self.assertIn('fragment main0_out_u main0_u(main0_in in [[stage_in]], ', fs_uint(self.source(body, extra)))

    def test_unknown_shapes_get_none(self):
        self.assertIsNone(fs_uint('struct main0_out\n{\n    float gl_FragDepth [[depth(any)]];\n};\n\nfragment main0_out main0()\n{\n    main0_out out = {};\n    return out;\n}\n'))   # no colour
        self.assertIsNone(fs_uint('kernel void main0() {}\n'))
        self.assertIsNone(fs_uint(self.source('').replace('fragment main0_out main0', 'fragment void main0')))

    def test_each_colour_switched_by_its_constants(self):   # Trails in the Sky 2nd Chapter's G-buffer: float at colour 0, integer at 1 to 3, whatever each shader writes there
        four = ''.join(f'    float4 m_{n} [[color({n})]];\n' for n in range(4)) + '    float gl_FragDepth [[depth(any)]];\n'
        body = ('    uint _342 = uint(_341);\n    out.m_0 = in.m_9 * 2.0;\n    out.m_1.x = as_type<float>(_342);\n    out.m_2.x = 0.0;\n'
                '    out.m_3.x = (*spvDescriptorSet0.fp_c8)._m0[2].w;\n    out.gl_FragDepth = 0.5;')
        src = f'struct main0_out\n{{\n{four}}};\n\nfragment main0_out main0(main0_in in [[stage_in]])\n{{\n    main0_out out = {{}};\n{body}\n    return out;\n}}\n'
        u = fs_uint(src)
        for n in range(4):
            self.assertIn(f'constant bool dm_ui{n} [[function_constant({dmsh_abi.UINT_FC + n})]];\nconstant bool dm_si{n} [[function_constant({dmsh_abi.SINT_FC + n})]];\n'
                          f'constant bool dm_uf{n} = !dm_ui{n} && !dm_si{n};\n', u)
            self.assertIn(f'    float4 m_{n} [[color({n}), function_constant(dm_uf{n})]];\n    uint4 m_{n}_ui [[color({n}), function_constant(dm_ui{n})]];\n'
                          f'    int4 m_{n}_si [[color({n}), function_constant(dm_si{n})]];\n', u)
            self.assertIn(f'    if (dm_ui{n}) r.m_{n}_ui = as_type<uint4>(out.m_{n}); else if (dm_si{n}) r.m_{n}_si = as_type<int4>(out.m_{n}); else r.m_{n} = out.m_{n};\n', u)   # the bits as they are
        self.assertIn('    float gl_FragDepth [[depth(any)]];\n};', u); self.assertIn('    r.gl_FragDepth = out.gl_FragDepth;\n', u)   # not a colour: as main0 has it
        self.assertIn('fragment main0_out_u main0_u(main0_in in [[stage_in]], ', u); self.assertIn(')\n{\n    main0_out out = dm_body_main0(in);\n', u)

    def test_narrow_and_declared_integer_colours(self):   # a narrow colour takes the vectors of its width; one main0 declares an integer (a fout fact) is typed too, a float target the reverse cast
        src = ('struct main0_out\n{\n    float2 m_0 [[color(0)]];\n    uint4 m_1 [[color(1)]];\n    int m_2 [[color(2)]];\n};\n\nfragment main0_out main0(main0_in in [[stage_in]])\n{\n    main0_out out = {};\n'
               '    out.m_0.x = as_type<float>(1u);\n    out.m_1 = uint4(2u);\n    out.m_2 = -3;\n    return out;\n}\n')
        u = fs_uint(src)
        self.assertIn('    uint2 m_0_ui [[color(0), function_constant(dm_ui0)]];\n    int2 m_0_si [[color(0), function_constant(dm_si0)]];\n', u)
        self.assertIn('    if (dm_ui1) r.m_1_ui = out.m_1; else if (dm_si1) r.m_1_si = as_type<int4>(out.m_1); else r.m_1 = as_type<float4>(out.m_1);\n', u)
        self.assertIn('    if (dm_ui2) r.m_2_ui = as_type<uint>(out.m_2); else if (dm_si2) r.m_2_si = out.m_2; else r.m_2 = as_type<float>(out.m_2);\n', u)

    def test_other_colour_shapes_stay(self):   # a half colour and one with a second index are kept as main0 declares them
        src = ('struct main0_out\n{\n    half4 m_0 [[color(0)]];\n    float4 m_1 [[color(1)]];\n};\n\nfragment main0_out main0()\n{\n    main0_out out = {};\n    return out;\n}\n')
        u = fs_uint(src)
        self.assertIn('    half4 m_0 [[color(0)]];\n    float4 m_1 [[color(1), function_constant(dm_uf1)]];\n', u)
        self.assertNotIn('dm_ui0', u); self.assertIn('    r.m_0 = out.m_0;\n', u)

class ImageAtomics(unittest.TestCase):
    MAGIC = [0x07230203, 0x10000, 0, 8, 0]
    NOP = [1 << 16]
    POINTER = [6 << 16 | 60, 1, 2, 3, 4, 5]

    def module(self, words):
        f = tempfile.NamedTemporaryFile(suffix='.spv', delete=False); f.write(struct.pack(f'<{len(words)}I', *words)); f.close()
        self.addCleanup(Path(f.name).unlink)
        return f.name

    def test_emulated_atomics_still_fail(self):
        for src in ('spvImage2DAtomicCoord(x)', 'device atomic_uint* t_atomic'):
            with self.assertRaises(ValueError): dmsh_atomic.check_msl(src)
        self.assertEqual(dmsh_atomic.check_msl('float4 x;'), 'float4 x;')


if __name__ == '__main__':
    unittest.main()
