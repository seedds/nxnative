"""Raw global memory lookups that reuse the range they last chose (dmsh_wide.py raw_ranges): the helper's search behind the cache, each call site's cache, shapes left
alone, and the compile."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from dmsh_wide import raw_ranges   # noqa: E402

# GlobalToStorage.TryGenerateRawStorageOp's load helper as spirv-cross writes it, its unrolled search cut to two steps, and a kernel that reads two neighbouring words
KERNEL = '''#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct _27
{
    uint _m0[1];
};

struct spvDescriptorSetBuffer0
{
    const device _27* cp_s3 [[id(0)]];
    device _27* cp_s4 [[id(1)]];
};

static inline __attribute__((always_inline))
int _56(thread const int& _100, thread const int& _101, const device _27& cp_s3)
{
    int _102 = _101 << 11;
    uint _103 = uint(int(uint(_100) >> uint(21)));
    int _104 = _102 | int(_103);
    int _105 = _104 & 8191;
    int _106 = _105 * 8;
    int _107 = _106 + 6;
    float _108 = as_type<float>(cp_s3._m0[_107]);
    int _109 = as_type<int>(_108) & 8191;
    bool _110 = _109 != 0;
    int _111 = _109;
    if (!_110)
    {
        int _112 = 0 + 4096;
        int _113 = _112 * 8;
        float _114 = as_type<float>(cp_s3._m0[_113]);
        bool _115 = as_type<uint>(_114) <= uint(_100);
        int _116 = _115 ? _112 : 0;
        int _117 = _116 + 2048;
        int _118 = _117 * 8;
        float _119 = as_type<float>(cp_s3._m0[_118]);
        bool _120 = as_type<uint>(_119) <= uint(_100);
        int _121 = _120 ? _117 : _116;
        _111 = _121;
    }
    int _122 = _111 * 8;
    int _123 = _122 + 2;
    float _124 = as_type<float>(cp_s3._m0[_123]);
    bool _125 = uint(_100) < as_type<uint>(_124);
    if (_125)
    {
        int _126 = _122 + 4;
        float _127 = as_type<float>(cp_s3._m0[_126]);
        int _128 = _100 + as_type<int>(_127);
        uint _129 = *(reinterpret_cast<const device uint*>(as_type<ulong>(uint2(uint(_128), uint(_101)))));
        float _130 = as_type<float>(_129);
        return as_type<int>(_130);
    }
    return 0;
}

kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], uint3 gl_GlobalInvocationID [[thread_position_in_grid]])
{
    int _200 = int(gl_GlobalInvocationID.x) * 8;
    int _201 = 0;
    int _202 = _56(_200, _201, (*spvDescriptorSet0.cp_s3));
    int _203 = _200 + 4;
    int _204 = _56(_203, _201, (*spvDescriptorSet0.cp_s3));
    (*spvDescriptorSet0.cp_s4)._m0[gl_GlobalInvocationID.x] = uint(_202 + _204);
}
'''


class RawRanges(unittest.TestCase):
    def test_search_behind_the_cache(self):
        out = raw_ranges(KERNEL)
        self.assertIn('struct dm_raw_range { ulong s; ulong e; int i; };', out)
        self.assertIn('int _56(thread const int& _100, thread const int& _101, const device _27& cp_s3, thread dm_raw_range& _rr)', out)
        helper = out[out.index('int _56('):out.index('kernel void main0')]
        # the hint and the search run only on a miss, and a miss records the chosen entry's range
        self.assertLess(helper.index('if (_ra - _rr.s < _rr.e - _rr.s) _111 = _rr.i;'), helper.index('float _108 = '))
        self.assertIn('        _111 = _109;', helper)
        self.assertIn('_rr.i = _111;', helper)
        # what follows the choice is unchanged
        self.assertIn('    int _122 = _111 * 8;\n    int _123 = _122 + 2;', helper)

    def test_call_sites_share_their_function_cache(self):
        out = raw_ranges(KERNEL)
        main = out[out.index('kernel void main0'):]
        self.assertEqual(main.count('dm_raw_range _rr_56 = {0ul, 0ul, 0};'), 1)
        self.assertIn('_56(_200, _201, (*spvDescriptorSet0.cp_s3), _rr_56)', main)
        self.assertIn('_56(_203, _201, (*spvDescriptorSet0.cp_s3), _rr_56)', main)

    def test_left_alone(self):
        self.assertIs(raw_ranges('kernel void main0() {}'), 'kernel void main0() {}')
        other = KERNEL.replace('    if (!_110)\n', '    if (_110)\n')   # not the helper's shape: the shader stays as it is
        self.assertIs(raw_ranges(other), other)

    @unittest.skipUnless(shutil.which('xcrun') and sys.platform == 'darwin', 'Metal compiler')
    def test_compiles(self):
        with tempfile.TemporaryDirectory() as d:
            m = os.path.join(d, 'k.metal')
            open(m, 'w').write(raw_ranges(KERNEL))
            r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance', m, '-o', os.path.join(d, 'k.air')],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr[-2000:])


if __name__ == '__main__':
    unittest.main()
