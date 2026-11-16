#!/usr/bin/env python3
"""One copy of a raw global memory lookup's search (dmsh_rewrite.py raw_noinline): the translator's dmsh_raw_find (GlobalToStorage GetRawFindFunction), as spirv-cross writes it, is
marked noinline; the access helpers that check the last entry and call it stay inline, and so does a function of the same shape without the name. The pass is idempotent, and with
xcrun the rewritten shader compiles. test_dmsh_raw_last.py runs the translator's own lookups on the GPU in both forms."""
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

struct _29
{
    uint _m0[1];
};
'''
INLINE, NOINLINE = 'static inline __attribute__((always_inline))', 'static __attribute__((noinline))'
# The search, shortened: the chunk's hint, else one step of the binary search
FIND = f'''
{INLINE}
int dmsh_raw_find(thread const int& _lo, thread const int& _hi, const device _29& cp_s2)
{{
    uint _h = cp_s2._m0[((uint(_lo) >> uint(21)) & 8191u) * 8u + 6u];
    if ((_h >> 13u) == (uint(_lo) >> uint(21)))
    {{
        return int(_h & 8191u);
    }}
    return (cp_s2._m0[4096 * 8] <= uint(_lo)) ? 4096 : 0;
}}
'''
# An access helper: the last entry's range, else the search; then the access through the entry's delta
def helper(name, find):
    return f'''
{INLINE}
int {name}(thread const int& _lo, thread const int& _hi, const device _29& cp_s2, thread uint& _last)
{{
    int _e = int(_last);
    if (cp_s2._m0[_e * 8] > uint(_lo))
    {{
        _e = {find}(_lo, _hi, cp_s2);
        _last = uint(_e);
    }}
    int _hl = _lo + int(cp_s2._m0[_e * 8 + 4]);
    int _hh = _hi + int(cp_s2._m0[_e * 8 + 5]) + int(uint(_hl) < uint(_lo));
    return int(*(reinterpret_cast<const device uint*>(as_type<ulong>(uint2(uint(_hl), uint(_hh))))));
}}
'''
OTHER = FIND.replace('dmsh_raw_find', '_70')   # the same search under a name the translator did not give
MAIN = '''
kernel void main0(const device _29& cp_s2 [[buffer(0)]], device uint* out [[buffer(1)]], uint gid [[thread_position_in_grid]])
{
    uint _last = 0u;
    int a = int(gid) * 4, b = 0, c = a + 16;
    out[gid] = uint(_63(a, b, cp_s2, _last) + _63(c, b, cp_s2, _last) + _64(a, b, cp_s2, _last) + _71(c, b, cp_s2, _last));
}
'''
SRC = HEAD + FIND + OTHER + helper('_63', 'dmsh_raw_find') + helper('_64', 'dmsh_raw_find') + helper('_71', '_70') + MAIN


def attribute(src, name):
    return re.search(r'\n([^\n]*)\nint %s\(' % name, src).group(1)


class RawNoinline(unittest.TestCase):
    def test_marks(self):
        out = raw_noinline(SRC)
        self.assertEqual(attribute(out, 'dmsh_raw_find'), NOINLINE)
        for name in ('_63', '_64', '_71'):
            self.assertEqual(attribute(out, name), INLINE, name)   # the last entry's check and the access stay at each call
        self.assertEqual(attribute(out, '_70'), INLINE, 'not named dmsh_raw_find')
        self.assertEqual(out.replace(NOINLINE, INLINE), SRC)

    def test_unchanged(self):
        plain = HEAD + OTHER + helper('_71', '_70') + MAIN
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
