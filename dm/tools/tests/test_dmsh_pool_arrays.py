"""dmsh_build.py pool_arrays and reflect on synthetic MSL: Ryujinx's arrays for a handle traced to no slot, sized to the whole texture / sampler ID ranges (Program.cs), become pointers to
the pools the replay keeps (nvn_mtl.m pool_gpu), declared with one element and read through a pointer to it with the run-time bound; the argument IDs are renumbered as the host lays the buffer out; 32-entry slot arrays stay; members with
attributes after their id (raster_order_group) are read; a member or a use the rewrite cannot read stops the shader instead of leaving the host's argument buffer longer than the reflection."""
import ast
from pathlib import Path
import re
import sys
import types
import unittest

TOOLS = Path(__file__).resolve().parents[2] / 'gfx/tools'
sys.path.insert(0, str(TOOLS))
import dmsh_abi as abi   # noqa: E402

FUNCS = {'decode_tex', 'const_sampler', 'decode_ubo_tex', 'decode_ubo_img', 'c2_array', 'pool_arrays', 'reflect', 'has_word', 'in_word'}
NAMES = {'POOL_MIN', 'POOL_MARK', 'TEX_BASE', 'SSBO'}


def load():
    tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
    nodes: list[ast.stmt] = [n for n in tree.body if (isinstance(n, ast.FunctionDef) and n.name in FUNCS)
             or (isinstance(n, ast.Assign) and any(isinstance(t, (ast.Name, ast.Tuple)) and {getattr(e, 'id', None) for e in (t.elts if isinstance(t, ast.Tuple) else [t])} & NAMES for t in n.targets))]
    ns = {'re': re, 'abi': abi, 'dmsh_geometry': types.SimpleNamespace(HELPERS={})}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), ns)
    assert FUNCS <= set(ns), FUNCS - set(ns)
    return ns


NS = load()
MSL = '''struct spvDescriptorSetBuffer0
{
    constant _10* support_buffer [[id(0)]];
    constant _16* fp_c5 [[id(1)]];
    array<texture2d<float>, 1048576> fp_t2d_cb0_40000000 [[id(2)]];
    array<sampler, 4096> fp_s_cb0_40000000 [[id(1048578)]];
    array<texture2d<float>, 32> fp_t2d_cb14_0 [[id(1052674)]];
    texture2d<uint> fp_i_cb2_48_rg32ui [[id(1052706), raster_order_group(0)]];
};

fragment main0_out main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
{
    float4 a = spvDescriptorSet0.fp_t2d_cb0_40000000[int(_9[2u])].sample(spvDescriptorSet0.fp_s_cb0_40000000[int(_7)], uv);
}
'''


class PoolArrays(unittest.TestCase):
    def test_rewrite(self):
        out = NS['pool_arrays'](MSL)
        self.assertIn('struct spvPool_fp_t2d_cb0_40000000\n{\n    uint n;\n    ulong seen;\n    array<texture2d<float>, 1> e;\n};', out)
        self.assertIn('struct spvPool_fp_s_cb0_40000000\n{\n    uint n;\n    ulong seen;\n    array<sampler, 1> e;\n};', out)
        self.assertLess(out.index('struct spvPool_'), out.index('struct spvDescriptorSetBuffer0'))
        ids = re.findall(r'(\w+) \[\[id\((\d+)\)', out)
        self.assertEqual(ids, [('support_buffer', '0'), ('fp_c5', '1'), ('fp_t2d_cb0_40000000', '2'), ('fp_s_cb0_40000000', '3'), ('fp_t2d_cb14_0', '4'), ('fp_i_cb2_48_rg32ui', '36')])
        self.assertIn('constant spvPool_fp_t2d_cb0_40000000* fp_t2d_cb0_40000000 [[id(2)]];', out)
        self.assertIn('fp_i_cb2_48_rg32ui [[id(36), raster_order_group(0)]];', out)
        self.assertIn('(&spvDescriptorSet0.fp_t2d_cb0_40000000->e[0])[dm_pool_mark(spvDescriptorSet0.fp_t2d_cb0_40000000, min(uint(int(_9[2u])), spvDescriptorSet0.fp_t2d_cb0_40000000->n))]', out)
        self.assertLess(out.index('dm_pool_mark(constant P* p'), out.index('struct spvPool_'))
        self.assertIn('.sample((&spvDescriptorSet0.fp_s_cb0_40000000->e[0])[min(uint(int(_7)), spvDescriptorSet0.fp_s_cb0_40000000->n)], uv)', out)

    def test_mark_keeps_depth_test_early(self):
        pool = 'struct spvDescriptorSetBuffer0\n{\n    array<texture2d<float>, 1048576> fp_t_cb0_40000000 [[id(0)]];\n};\n'
        use = 'fragment main0_out main0(constant spvDescriptorSetBuffer0& d [[buffer(0)]])\n{\n    float4 c = d.fp_t_cb0_40000000[i].sample(s, u);%s\n}\n'
        out = NS['pool_arrays'](pool + use % '')
        self.assertIn('[[early_fragment_tests]] fragment main0_out main0(', out)   # the mark is its only store
        for keeps_late in (' discard_fragment();', ' out.gl_FragDepth [[depth(any)]];', ' d.img.write(c, p);', ' atomic_fetch_add_explicit(q, 1u, memory_order_relaxed);', '\n    device uint* q [[id(9)]];'):
            self.assertNotIn('early_fragment_tests', NS['pool_arrays'](pool + use % keeps_late))
        twice = NS['pool_arrays'](pool + '[[early_fragment_tests]] ' + use % '')
        self.assertEqual(twice.count('early_fragment_tests'), 1)

    def test_slot_arrays_stay(self):
        src = MSL.replace('1048576', '32').replace('4096', '32')
        self.assertEqual(NS['pool_arrays'](src), src)

    def test_uncovered_use(self):
        with self.assertRaises(ValueError): NS['pool_arrays'](MSL.replace('    float4 a', '    f(spvDescriptorSet0.fp_t2d_cb0_40000000);\n    float4 a'))


class Reflect(unittest.TestCase):
    def test_pools_and_attributes(self):
        b = {e['msl_name']: e for e in NS['reflect']('k', 'ps', NS['pool_arrays'](MSL), {'sbufs': []})}
        self.assertEqual((b['fp_t2d_cb0_40000000']['kind'], b['fp_t2d_cb0_40000000']['elem'], b['fp_t2d_cb0_40000000']['arg_id']), (abi.KIND_TEX_POOL, 'texture2d<float>', 2))
        self.assertEqual((b['fp_s_cb0_40000000']['kind'], b['fp_s_cb0_40000000']['arg_id']), (abi.KIND_SAMP_POOL, 3))
        self.assertEqual((b['fp_i_cb2_48_rg32ui']['kind'], b['fp_i_cb2_48_rg32ui']['reg'], b['fp_i_cb2_48_rg32ui']['arg_id']), (abi.KIND_IMG, 0, 36))

    def test_table_of_uniform_handles(self):   # Kirby Air Riders' light probes: texture words in c[4] at element 390, the sampler word at c2 byte 0x570; the index stays the element's
        src = '''struct spvDescriptorSetBuffer0
{
    array<texture2d_array<float>, 8192> cp_t2da_cb196612_10570000 [[id(0)]];
    array<sampler, 8192> cp_t2da_cb196612_10570000Smplr [[id(8192)]];
    array<texture2d<float>, 32> fp_t2d_cb14_0 [[id(16384)]];
};

kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
{
    float3 v = spvDescriptorSet0.cp_t2da_cb196612_10570000[_7].sample(spvDescriptorSet0.cp_t2da_cb196612_10570000Smplr[_7], p, 0u, level(0.0)).xyz;
}
'''
        out = NS['pool_arrays'](src)
        self.assertIn('(&spvDescriptorSet0.cp_t2da_cb196612_10570000->e[0])[min(uint(_7), spvDescriptorSet0.cp_t2da_cb196612_10570000->n)]', out)   # a table's index is its element, not a texture ID: not marked
        self.assertNotIn('dm_pool_mark', out)
        self.assertNotIn('early_fragment_tests', NS['pool_arrays'](src.replace('kernel void', 'fragment main0_out')))   # nothing marked, nothing stored: the test stays where Metal puts it
        self.assertIn('(&spvDescriptorSet0.cp_t2da_cb196612_10570000Smplr->e[0])[min(uint(_7), spvDescriptorSet0.cp_t2da_cb196612_10570000Smplr->n)]', out)   # a sampler read does not
        self.assertIn('array<texture2d<float>, 32> fp_t2d_cb14_0 [[id(2)]];', out)   # a pointer takes one ID; an array of 32 stays an array (nothing in the game's buffers is that short now but images and buffer textures)
        b = {e['msl_name']: e for e in NS['reflect']('k', 'cs', out, {'sbufs': []})}
        t, s = b['cp_t2da_cb196612_10570000'], b['cp_t2da_cb196612_10570000Smplr']
        self.assertEqual((t['kind'], t['elem'], t['reg'], t['off'], t['arg_id'], t.get('soff')), (abi.KIND_TEX_HTAB, 'texture2d_array<float>', 1, 0, 0, 0x570))
        self.assertEqual((s['kind'], s['reg'], s['off'], s['arg_id'], s['soff'], s['sreg']), (abi.KIND_SAMP_HTAB, 1, 0, 1, 0x570, abi.SREG_C2))

    def test_unreadable_member(self):
        with self.assertRaises(ValueError): NS['reflect']('k', 'ps', MSL.replace('raster_order_group(0)', 'raster_order_group'), {'sbufs': []})


if __name__ == '__main__':
    unittest.main()
