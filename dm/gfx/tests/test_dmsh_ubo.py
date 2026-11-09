"""Uniform reads retain the source index and take the NVN byte length from the argument block."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import cb_extent
import dmsh_ubo

SOURCE = '''#include <metal_stdlib>
using namespace metal;
struct Bank { float4 _m0[4096]; };
struct spvDescriptorSetBuffer0 {
    constant Bank* fp_c14 [[id(0)]];
};
vertex float4 tri(uint v [[vertex_id]]) {
    return float4(v == 1 ? 3.0 : -1.0, v == 2 ? 3.0 : -1.0, 0, 1);
}
fragment uint4 read_bank(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]]) {
    return uint4(as_type<uint>((*spvDescriptorSet0.fp_c14)._m0[0].x),
                 as_type<uint>((*spvDescriptorSet0.fp_c14)._m0[144].x),
                 as_type<uint>((*spvDescriptorSet0.fp_c14)._m0[144].y), 0);
}
'''

class UniformBounds(unittest.TestCase):
    def test_extents(self):
        bindings = [{'kind': 'cbuf', 'reg': 11, 'msl_name': 'fp_c14'}]
        self.assertEqual(cb_extent.extents(dmsh_ubo.bounded_fragment(SOURCE), bindings), {11: 2320})

    def test_embedded_constants_unchanged(self):
        raw = SOURCE.replace('fp_c14', 'fp_c1')
        self.assertEqual(dmsh_ubo.bounded_fragment(raw), raw)

    def test_index_and_array(self):
        raw = SOURCE.replace(' [[id(0)]];', ' [[id(0)]];\n    array<texture2d<float>, 4> tex [[id(1)]];')
        raw = raw.replace('[144]', '[index[n]]')
        guarded = dmsh_ubo.bounded_fragment(raw)
        self.assertIn('dm_ubo_bytes_fp_c14 [[id(5)]]', guarded)
        self.assertIn('uint(index[n])', guarded)

if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--gpu':
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'fragment.metal'
            path.write_text(dmsh_ubo.bounded_fragment(SOURCE))
            subprocess.run([sys.argv[2], str(path)], check=True)
    else:
        unittest.main()
