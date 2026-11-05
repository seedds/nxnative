"""Storage words the translator split, read whole again (dmsh_wide.py wide_loads): which words become one read, which are left alone, and the compile."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import dmsh_chain  # noqa: E402
from dmsh_wide import raw_load_quads, wide_loads   # noqa: E402

# Splatoon Raiders' Hoian_UBER (3e5a302ac52b73bf) as spirv-cross writes it, cut down: one light's words from fp_s3 at 80 * index + 16 .. 44, a written buffer,
# and words in two blocks
LIGHT = '''#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct _29
{
    uint _m0[1];
};

struct spvDescriptorSetBuffer0
{
    const device _29* fp_s2 [[id(0)]];
    const device _29* fp_s3 [[id(1)]];
    device _29* fp_s4 [[id(2)]];
};

struct main0_out
{
    float4 m_0 [[color(0)]];
};

struct main0_in
{
    float4 m_1 [[user(locn0)]];
};

fragment main0_out main0(main0_in in [[stage_in]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
{
    main0_out out = {};
    int _3483 = int(in.m_1.x) * 80;
    int _3485 = _3483 + 16;
    int _3487 = _3483 + 32;
    uint _3493 = uint(int(uint(_3485) >> uint(2)));
    float _3495 = as_type<float>((*spvDescriptorSet0.fp_s3)._m0[int(_3493)]);
    uint _3499 = uint(int(uint(_3487) >> uint(2)));
    float _3501 = as_type<float>((*spvDescriptorSet0.fp_s3)._m0[int(_3499)]);
    int _3503 = _3487 + 4;
    uint _3505 = uint(int(uint(_3503) >> uint(2)));
    float _3507 = as_type<float>((*spvDescriptorSet0.fp_s3)._m0[int(_3505)]);
    int _3509 = _3487 + 8;
    uint _3511 = uint(int(uint(_3509) >> uint(2)));
    float _3513 = as_type<float>((*spvDescriptorSet0.fp_s3)._m0[int(_3511)]);
    int _3515 = _3487 + 12;
    uint _3517 = uint(int(uint(_3515) >> uint(2)));
    uint _3519 = (*spvDescriptorSet0.fp_s3)._m0[int(_3517)];
    uint _20 = uint(int(uint(_3483) >> uint(2)));
    uint _21 = (*spvDescriptorSet0.fp_s4)._m0[int(_20)];
    int _22 = _3483 + 4;
    uint _23 = uint(int(uint(_22) >> uint(2)));
    uint _24 = (*spvDescriptorSet0.fp_s4)._m0[int(_23)];
    int _30 = _3483 + 64;
    int _31 = _3483 + 68;
    uint _32 = uint(int(uint(_30) >> uint(2)));
    uint _33 = uint(int(uint(_31) >> uint(2)));
    uint _34 = (*spvDescriptorSet0.fp_s2)._m0[int(_33)];
    uint _35 = (*spvDescriptorSet0.fp_s2)._m0[int(_32)];
    int _40 = _3483 + 48;
    uint _41 = uint(int(uint(_40) >> uint(2)));
    uint _42 = (*spvDescriptorSet0.fp_s2)._m0[int(_41)];
    if (_3495 > 0.0)
    {
        int _43 = _40 + 4;
        uint _44 = uint(int(uint(_43) >> uint(2)));
        _42 += (*spvDescriptorSet0.fp_s2)._m0[int(_44)];
    }
    out.m_0 = float4(_3495 + _3501, _3507 + _3513, as_type<float>(_3519 + _21 + _24), as_type<float>(_34 + _35 + _42));
    return out;
}
'''


class WideLoads(unittest.TestCase):
    def test_words_of_one_light(self):
        out = wide_loads(LIGHT)
        # bytes 32..44 from one value: one packed_uint4 at the first word, each word its lane (float or uint alike)
        self.assertIn('packed_uint4 _dmw0 = *(const device packed_uint4*)&(*spvDescriptorSet0.fp_s3)._m0[int(_3499)];', out)
        for lane, var in zip('xyz', ('_3501', '_3507', '_3513')): self.assertIn(f'float {var} = as_type<float>(_dmw0.{lane});', out)
        self.assertIn('uint _3519 = _dmw0.w;', out)
        # byte 16 is alone on its 16-byte step: as it was
        self.assertIn('float _3495 = as_type<float>((*spvDescriptorSet0.fp_s3)._m0[int(_3493)]);', out)
        self.assertEqual(out.count('_dmw'), 5)   # one declaration, four lanes

    def test_left_alone(self):
        out = wide_loads(LIGHT)
        self.assertIn('(*spvDescriptorSet0.fp_s4)._m0[int(_20)]', out)   # a buffer the shader may write
        self.assertIn('(*spvDescriptorSet0.fp_s4)._m0[int(_23)]', out)
        self.assertIn('(*spvDescriptorSet0.fp_s2)._m0[int(_33)]', out)   # the higher word read first
        self.assertIn('(*spvDescriptorSet0.fp_s2)._m0[int(_32)]', out)
        self.assertIn('(*spvDescriptorSet0.fp_s2)._m0[int(_41)]', out)   # its neighbour is in another block
        self.assertIn('(*spvDescriptorSet0.fp_s2)._m0[int(_44)]', out)

    def test_writes_end_a_run(self):
        # a store through another binding (which may be the same memory), an atomic, a barrier or a new common value between the words: each word read where it was
        first = '    float _3501 = as_type<float>((*spvDescriptorSet0.fp_s3)._m0[int(_3499)]);\n'
        for between in ('    (*spvDescriptorSet0.fp_s4)._m0[int(_20)] = 1u;', '    (*spvDescriptorSet0.fp_s4)._m0[int(_20)] += 1u;',
                        '    uint _90 = atomic_fetch_add_explicit((device atomic_uint*)&(*spvDescriptorSet0.fp_s4)._m0[int(_20)], 1u, memory_order_relaxed);',
                        '    threadgroup_barrier(mem_flags::mem_device);', '    _3487 = _3483 + 48;'):
            out = wide_loads(LIGHT.replace(first, first + between + '\n'))
            self.assertNotIn('_dmw', out, between)
            self.assertIn('(*spvDescriptorSet0.fp_s3)._m0[int(_3505)]', out)

    def test_nothing_to_do(self):
        plain = LIGHT.replace('const device', 'device')
        self.assertEqual(wide_loads(plain), plain)

    @unittest.skipUnless(shutil.which('xcrun') and sys.platform == 'darwin', 'Metal compiler')
    def test_compiles(self):
        with tempfile.TemporaryDirectory() as d:
            for name, src in (('before', LIGHT), ('after', wide_loads(LIGHT))):
                m = os.path.join(d, name + '.metal'); Path(m).write_text(src)
                r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance', m, '-o', os.path.join(d, name + '.air')],
                                   capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr)


def raw_source(entries):
    """Synthetic fixed-table helper and four words of a raw 128-bit load."""
    prefix = '''
struct _10 { uint _m0[1]; };
struct spvDescriptorSetBuffer0
{
    device _10* cp_s5 [[id(7)]];
};
static inline __attribute__((always_inline))
int _900(thread const int& _1, thread const int& _2, device _10& cp_s5)
{
'''
    next_id = 100
    previous = '0'
    for bit in reversed(range(entries.bit_length() - 1)):
        probe, row, li, low, hi, high, less, equal, lo_le, tied, take, index = (
            f'_{i}' for i in range(next_id, next_id + 12))
        prefix += f'''    int {probe} = {previous} + {1 << bit};
    int {row} = {probe} * 8;
    int {li} = {row} + 0;
    float {low} = as_type<float>(cp_s5._m0[{li}]);
    int {hi} = {row} + 1;
    float {high} = as_type<float>(cp_s5._m0[{hi}]);
    bool {less} = as_type<uint>({high}) < uint(_2);
    bool {equal} = as_type<int>({high}) == _2;
    bool {lo_le} = as_type<uint>({low}) <= uint(_1);
    bool {tied} = {equal} && {lo_le};
    bool {take} = {less} || {tied};
    int {index} = {take} ? {probe} : {previous};
'''
        previous = index
        next_id += 12
    prefix += f'''    int _310 = {previous} * 8;
    int _311 = _310 + 2;
    float _312 = as_type<float>(cp_s5._m0[_311]);
    int _313 = _310 + 3;
    float _314 = as_type<float>(cp_s5._m0[_313]);
    bool _315 = uint(_2) < as_type<uint>(_314);
    bool _316 = _2 == as_type<int>(_314);
    bool _317 = uint(_1) < as_type<uint>(_312);
    bool _318 = _316 && _317;
    bool _319 = _315 || _318;
    if (_319)
    {{
        int _320 = _310 + 4;
        float _321 = as_type<float>(cp_s5._m0[_320]);
        int _322 = _1 + as_type<int>(_321);
        bool _323 = uint(_322) < uint(_1);
        int _324 = int(_323);
        int _325 = _310 + 5;
        float _326 = as_type<float>(cp_s5._m0[_325]);
        int _327 = _2 + as_type<int>(_326);
        int _328 = _327 + _324;
        uint _329 = *(reinterpret_cast<device uint*>(as_type<ulong>(uint2(uint(_322), uint(_328)))));
        float _330 = as_type<float>(_329);
        return as_type<int>(_330);
    }}
    return 0;
}}
kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], uint id [[thread_position_in_grid]])
{{
    int _500 = int(id);
    int _501 = 4;
'''
    for i, offset in enumerate((4, 8, 12)):
        lo, wraps, carry, hi = (500 + 4 * (i + 1) + j for j in range(4))
        prefix += f'''    int _{lo} = _500 + {offset};
    bool _{wraps} = uint(_{lo}) < uint(_500);
    int _{carry} = int(_{wraps});
    int _{hi} = _501 + _{carry};
'''
    for i, (lo, hi) in enumerate(((500, 501), (504, 507), (508, 511), (512, 515))):
        low, high, out = 600 + i * 3, 601 + i * 3, 602 + i * 3
        prefix += f'''    int _{low} = _{lo};
    int _{high} = _{hi};
    int _{out} = _900(_{low}, _{high}, (*spvDescriptorSet0.cp_s5));
'''
    return prefix + '}\n'


class RawLoadQuads(unittest.TestCase):
    def test_opt_in_changes_chain(self):
        with patch.dict(os.environ, {}, clear=True):
            self.assertFalse(dmsh_chain.raw_load_quads())
            command = [sys.executable, dmsh_chain.__file__]
            original = subprocess.check_output(command)
            os.environ['DMSH_RAW_LOAD_QUADS'] = '1'
            self.assertTrue(dmsh_chain.raw_load_quads())
            self.assertNotEqual(subprocess.check_output(command), original)

    def test_known_layouts(self):
        for entries in (4096, 8192):
            with self.subTest(entries=entries):
                stock = raw_source(entries)
                result = raw_load_quads(stock, 5, entries, 32)
                self.assertIn(f'index < {entries - 1}u ?', result)
                self.assertIn('next > address + 15ul', result)
                self.assertIn('end - address >= 16ul', result)
                self.assertIn('dm_raw_load4(_600, _601, (*spvDescriptorSet0.cp_s5))', result)
                for ident, component in zip((602, 605, 608, 611), 'xyzw'):
                    self.assertIn(f'int _{ident} = _602_words.{component};', result)
                self.assertEqual(result.count('return _900(low, high, table);'), 1)
                self.assertEqual(result.count('_words = dm_raw_load4'), 1)
                self.assertEqual(raw_load_quads(result, 5, entries, 32), result)

    def test_unrecognized_layout_stays_scalar(self):
        stock = raw_source(4096)
        for slot, entries, entry_bytes in ((4, 4096, 32), (5, 8192, 32), (5, 4096, 16), (5, 7, 32)):
            self.assertEqual(raw_load_quads(stock, slot, entries, entry_bytes), stock)
        for old, new in (('uint _m0[1]', 'float _m0[1]'),
                         ('device _10* cp_s5 [[id(7)]]', 'device _11* cp_s5 [[id(7)]]'),
                         ('return 0;', 'return 1;'),
                         ('< uint(_2);', '<= uint(_2);'),
                         ('int _320 = _310 + 4;', 'int _320 = _310 + 5;')):
            bad = stock.replace(old, new, 1)
            self.assertNotEqual(bad, stock)
            self.assertEqual(raw_load_quads(bad, 5, 4096, 32), bad)

    def test_unproven_words_stay_scalar(self):
        stock = raw_source(4096)
        for old, new in (('int _504 = _500 + 4;', 'int _504 = _500 + 6;'),
                         ('uint(_504) < uint(_500)', 'uint(_504) <= uint(_500)'),
                         ('int _500 = int(id);', 'float _500 = float(id);'),
                         ('int _603 = _504;', 'unknown_effect(_504);\n    int _603 = _504;'),
                         ('int _603 = _504;', 'threadgroup_barrier(mem_flags::mem_device);\n    int _603 = _504;'),
                         ('int _603 = _504;', 'table[0] = 7;\n    int _603 = _504;'),
                         ('int _600 = _500;', 'int _999 = (_500 += 1);\n    int _600 = _500;'),
                         ('int _600 = _500;', 'int _999 = unknown_effect(_500);\n    int _600 = _500;'),
                         ('_900(_603, _604, (*spvDescriptorSet0.cp_s5))',
                          '_900(_603, _604, (*spvDescriptorSet0.cp_s3))')):
            bad = stock.replace(old, new, 1)
            with self.subTest(change=new):
                self.assertNotEqual(bad, stock)
                self.assertEqual(raw_load_quads(bad, 5, 4096, 32), bad)


if __name__ == '__main__':
    unittest.main()
