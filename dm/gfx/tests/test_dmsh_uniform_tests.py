"""A game uniform word tested against constants (dmsh_rewrite.py uniform_tests): each test of a word tested more than once reads it with a volatile load (dm_uword), so the Metal
front end folds no chain of tests into a switch, which Apple's GPU compiler took hours and tens of gigabytes over (Nintendo Switch Sports Resort's pixel shader 692008b305ce264f in
the offline pipeline archive). The shader below is the translator's form of a GLSL switch on one word; with dmsh_build.py's and ios_metal.py's compile commands its plain form
becomes a switch in the front end's IR and its rewritten form none."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
TOOLS = ROOT / 'dm/gfx/tools'
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(ROOT / 'dm/ios/tools'))
import dmsh_chain  # noqa: E402
import dmsh_rewrite  # noqa: E402
import ios_metal  # noqa: E402

WORD = '(*spvDescriptorSet0.fp_c5)._m0[16]'


# As spirv-cross writes Ryujinx's structured switch on c5[16].w (the cases 100 to 103 of a material's mode), cut down; the last test with the constant first
SHADER = '''#include <metal_stdlib>
using namespace metal;

struct _16
{
    float4 _m0[4096];
};

struct spvDescriptorSetBuffer0
{
    constant _16* fp_c5 [[id(0)]];
};

struct main0_out
{
    float4 out_var_SV_Target [[color(0)]];
};

fragment main0_out main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], float4 gl_FragCoord [[position]])
{
    main0_out out = {};
    float _10 = 0.0;
    bool _11 = as_type<int>(%(w)s.w) != 100;
    if (!_11)
    {
        _10 = gl_FragCoord.x;
    }
    else
    {
        bool _12 = as_type<int>(%(w)s.w) != 101;
        if (!_12)
        {
            _10 = sin(gl_FragCoord.y);
        }
        else
        {
            bool _13 = as_type<int>(%(w)s.w) != 102;
            if (!_13)
            {
                _10 = cos(gl_FragCoord.x);
            }
            else
            {
                bool _14 = 103 == as_type<int>(%(w)s.w);
                if (_14)
                {
                    _10 = exp2(gl_FragCoord.y);
                }
            }
        }
    }
    out.out_var_SV_Target = float4(_10 * %(w)s.x);
    return out;
}
''' % {'w': WORD}


class UniformTests(unittest.TestCase):
    def test_tested_words_are_volatile_loads(self):
        got = dmsh_rewrite.uniform_tests(SHADER)
        self.assertEqual(got.count('as_type<int>(dm_uword(%s, 3))' % WORD), 4, 'every test, the constant on either side')
        self.assertIn('_10 * %s.x' % WORD, got, 'a read in arithmetic is left as it is')
        self.assertEqual(got.count('#define DM_UWORD'), 1)
        self.assertLess(got.index('dm_uword(const constant'), got.index('struct _16'), 'the helper before the shader')
        self.assertEqual(dmsh_rewrite.uniform_tests(got), got, 'a second pass changes nothing')

    def test_left_as_it_is(self):
        once = 'using namespace metal;\nbool _1 = as_type<int>(%s.w) != 100;\nbool _2 = as_type<int>(%s.z) != 100;\n' % (WORD, WORD)
        self.assertEqual(dmsh_rewrite.uniform_tests(once), once, 'each word tested once: no chain')
        other = 'using namespace metal;\nbool _1 = as_type<int>(%s.w) != _5;\nbool _2 = as_type<int>(%s.w) >= 3;\nbool _3 = _6 == as_type<int>(%s.w);\n' % (WORD, WORD, WORD)
        self.assertEqual(dmsh_rewrite.uniform_tests(other), other, 'tests against no constant, or not of equality')
        self.assertEqual(dmsh_rewrite.uniform_tests('bool _1 = _2 != 3;'), 'bool _1 = _2 != 3;', 'no descriptor set')
        for bank in (0, 1, 2):   # the driver's banks: the host fills them, and dmsh_entry.vs_base_instance looks for the read of c0 row 100 as written
            drv = 'using namespace metal;\nbool _1 = as_type<int>((*spvDescriptorSet0.vp_c%d)._m0[100].y) != 0;\nbool _2 = as_type<int>((*spvDescriptorSet0.vp_c%d)._m0[100].y) != 1;\n' % (bank, bank)
            self.assertEqual(dmsh_rewrite.uniform_tests(drv), drv, 'a driver bank, c%d' % bank)

    def test_unsigned_and_negative(self):
        src = 'using namespace metal;\nbool _1 = as_type<uint>(%s.y) == 4u;\nbool _2 = as_type<uint>(%s.y) != 5u;\nbool _3 = -1 != as_type<int>(%s.x);\nbool _4 = as_type<int>(%s.x) == -2;\n' % ((WORD,) * 4)
        got = dmsh_rewrite.uniform_tests(src)
        self.assertEqual(got.count('as_type<uint>(dm_uword(%s, 1))' % WORD), 2)
        self.assertEqual(got.count('as_type<int>(dm_uword(%s, 0))' % WORD), 2)

    def test_needs_the_namespace(self):
        with self.assertRaisesRegex(ValueError, 'using namespace metal'): dmsh_rewrite.uniform_tests(SHADER.replace('using namespace metal;\n', ''))

    def switches(self, command, source):   # the front end's IR of source with command (its -c compile, emitting LLVM IR instead): how many switch instructions main0 has
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'test.metal').write_text(source)
            r = subprocess.run(command + ['-w', '-S', '-emit-llvm', str(root / 'test.metal'), '-o', str(root / 'test.ll')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            return (root / 'test.ll').read_text().count(' switch ')

    @unittest.skipUnless(shutil.which('xcrun'), 'needs Xcode and Metal')
    def test_no_switch_for_the_gpu_compiler(self):
        for name, command in (('Mac', dmsh_chain.MAC_METAL), ('iPhone', ios_metal.METAL)):
            with self.subTest(platform=name):
                self.assertGreater(self.switches(command, SHADER), 0, 'the plain tests fold into a switch: the test no longer shows the problem')
                self.assertEqual(self.switches(command, dmsh_rewrite.uniform_tests(SHADER)), 0)


if __name__ == '__main__':
    unittest.main()
