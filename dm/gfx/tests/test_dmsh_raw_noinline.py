#!/usr/bin/env python3
"""One copy of each raw global memory lookup (dmsh_rewrite.py raw_noinline): a pool table helper as spirv-cross writes it (TryGenerateRawStorageOp's scalar and vector loads, and a
store) called more than once is marked noinline, one called once stays inline, and a function of the same parameters that makes no physical pointer access is left alone. With xcrun,
the rewritten shader compiles."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
from dmsh_rewrite import raw_noinline   # noqa: E402

HEAD = '''#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];

    thread T& operator [] (size_t pos) thread { return elements[pos]; }
    constexpr const thread T& operator [] (size_t pos) const thread { return elements[pos]; }
};

struct _29
{
    uint _m0[1];
};
'''
# The lookup's shape, shortened: the last entry's range, else a search (here one step), then the access through the entry's delta
LOOKUP = '''    int _e = int(_last);
    if (as_type<uint>(cp_s2._m0[_e * 8]) > uint(_lo))
    {{
        _e = int(cp_s2._m0[6] & 8191u);
        _last = uint(_e);
    }}
    int _hl = _lo + int(cp_s2._m0[_e * 8 + 4]);
    int _hh = _hi + int(cp_s2._m0[_e * 8 + 5]) + int(uint(_hl) < uint(_lo));
{access}
'''
def helper(name, params, ret, access):
    return f'\nstatic inline __attribute__((always_inline))\n{ret} {name}({params})\n{{\n' + LOOKUP.format(access=access) + '}\n'

SCALAR = helper('_63', 'thread const int& _lo, thread const int& _hi, const device _29& cp_s2, thread uint& _last', 'int',
                '    return int(*(reinterpret_cast<const device uint*>(as_type<ulong>(uint2(uint(_hl), uint(_hh))))));')
VECTOR = helper('_64', 'thread const int& _lo, thread const int& _hi, const device _29& cp_s2, thread uint& _last, thread spvUnsafeArray<uint, 3>& _vec',
                'int', '    uint4 _v = *(reinterpret_cast<const device uint4*>(as_type<ulong>(uint2(uint(_hl), uint(_hh)))));\n'
                '    _vec[0] = _v.y; _vec[1] = _v.z; _vec[2] = _v.w;\n    return int(_v.x);')
STORE = helper('_65', 'thread const int& _lo, thread const int& _hi, thread const int& _val, const device _29& cp_s2, thread uint& _last', 'void',
               '    *(reinterpret_cast<device uint*>(as_type<ulong>(uint2(uint(_hl), uint(_hh))))) = uint(_val);')
ONCE = helper('_66', 'thread const int& _lo, thread const int& _hi, const device _29& cp_s2, thread uint& _last', 'int',
              '    return int(*(reinterpret_cast<const device uint*>(as_type<ulong>(uint2(uint(_hl), uint(_hh))))));')
PLAIN = '''
static inline __attribute__((always_inline))
int _67(thread const int& _lo, thread const int& _hi, const device _29& cp_s2, thread uint& _last)
{
    return int(cp_s2._m0[_lo + _hi]) + int(_last);
}
'''
MAIN = '''
kernel void main0(const device _29& cp_s2 [[buffer(0)]], device uint* out [[buffer(1)]], uint gid [[thread_position_in_grid]])
{
    uint _last = 0u;
    spvUnsafeArray<uint, 3> _vec;
    int a = int(gid) * 4, b = 0, c = a + 16;
    int r = _63(a, b, cp_s2, _last) + _63(c, b, cp_s2, _last);
    r += _64(a, b, cp_s2, _last, _vec) + _64(c, b, cp_s2, _last, _vec) + int(_vec[0]);
    _65(a, b, r, cp_s2, _last);
    _65(c, b, r, cp_s2, _last);
    r += _66(a, b, cp_s2, _last) + _67(a, b, cp_s2, _last) + _67(c, b, cp_s2, _last);
    out[gid] = uint(r);
}
'''
SRC = HEAD + SCALAR + VECTOR + STORE + ONCE + PLAIN + MAIN
INLINE, NOINLINE = 'static inline __attribute__((always_inline))', 'static __attribute__((noinline))'

def attribute(src, name):
    return re.search(r'\n([^\n]*)\n(?:int|uint|void) %s\(' % name, src).group(1)

class RawNoinline(unittest.TestCase):
    def test_marks(self):
        out = raw_noinline(SRC)
        for name in ('_63', '_64', '_65'):
            self.assertEqual(attribute(out, name), NOINLINE, name)
        self.assertEqual(attribute(out, '_66'), INLINE, 'called once')
        self.assertEqual(attribute(out, '_67'), INLINE, 'no physical pointer access')
        self.assertEqual(out.replace(NOINLINE, INLINE), SRC)

    def test_unchanged(self):
        plain = HEAD + PLAIN + MAIN.replace('_63', '_67')
        self.assertEqual(raw_noinline(plain), plain)
        self.assertEqual(raw_noinline(raw_noinline(SRC)), raw_noinline(SRC))

    @unittest.skipUnless(shutil.which('xcrun'), 'no xcrun')
    def test_compiles(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'raw.metal'; p.write_text(raw_noinline(SRC))
            r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', str(p), '-o', str(Path(d) / 'raw.air')], capture_output=True, text=True)
            if 'unable to find utility' in r.stderr: self.skipTest('no Metal toolchain')
            self.assertEqual(r.returncode, 0, r.stderr)

if __name__ == '__main__':
    unittest.main()
